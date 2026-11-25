"""Prepare, upload, and atomically publish GitHub Release assets with retries.

An existing same-name asset must have the same SHA-256. Different bytes require
a new release version. Incomplete uploads use a temporary name, so an interrupted
transfer cannot remove the download users already have.

Supports zero-bounce Draft Releases:
  1. --prepare-draft <SHA> creates (or reuses for the same SHA) a hidden draft
     release and prints its numeric release_id.
  2. --asset <PATH> --release-id <ID> streams the asset directly from the build
     runner to uploads.github.com (no Actions Artifact bounce, no disk copy)
     and verifies size/state/SHA-256 before renaming to its final filename.
  3. --publish-draft <ID> --expect-asset <NAME>... verifies every expected asset
     is uploaded and flips draft=false atomically.
"""
import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
from urllib.parse import quote


class GitHub:
    def __init__(self, repo):
        self.repo = repo

    def api(self, method, path, payload=None, **fields):
        command = ["gh", "api", "--method", method, f"repos/{self.repo}/{path}"]
        stdin_text = None
        if payload is not None:
            command.extend(["--input", "-"])
            stdin_text = json.dumps(payload)
        else:
            for key, value in fields.items():
                command.extend(["-f", f"{key}={value}"])
        result = subprocess.run(
            command,
            input=stdin_text,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=60,
        )
        return json.loads(result.stdout) if result.stdout.strip() else None

    def upload(self, tag, path):
        # Only the private temporary name is replaceable, never the published name.
        subprocess.run(["gh", "release", "upload", tag, str(path), "--repo", self.repo,
                        "--clobber"], check=True, timeout=300)

    def upload_by_id(self, release_id, asset_name, path):
        token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
        if not token:
            raise RuntimeError("GH_TOKEN or GITHUB_TOKEN is required for direct release_id uploads")
        size = path.stat().st_size
        endpoint = f"/repos/{self.repo}/releases/{release_id}/assets?name={quote(asset_name, safe='')}"
        conn = http.client.HTTPSConnection("uploads.github.com", timeout=300)
        try:
            conn.putrequest("POST", endpoint)
            conn.putheader("Authorization", f"Bearer {token}")
            conn.putheader("Accept", "application/vnd.github+json")
            conn.putheader("X-GitHub-Api-Version", "2022-11-28")
            conn.putheader("Content-Type", "application/octet-stream")
            conn.putheader("Content-Length", str(size))
            conn.putheader("User-Agent", "OptiScalerNR-ReleaseUploader")
            conn.endheaders()
            with path.open("rb") as f:
                while True:
                    chunk = f.read(1024 * 1024)
                    if not chunk:
                        break
                    conn.send(chunk)
            resp = conn.getresponse()
            body = resp.read().decode("utf-8", errors="replace")
            if resp.status not in (200, 201):
                raise OSError(f"GitHub asset upload failed: HTTP {resp.status}: {body[:400]}")
            return json.loads(body) if body.strip() else None
        finally:
            conn.close()


def retry(operation, attempts, sleep):
    for attempt in range(attempts):
        try:
            return operation()
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError, http.client.HTTPException):
            if attempt + 1 == attempts:
                raise
            delay = min(30, 2 ** (attempt + 1))
            print(f"GitHub transfer/API failed; retry {attempt + 2}/{attempts} in {delay}s", file=sys.stderr, flush=True)
            sleep(delay)


def verify_release_source(client, tag, sha):
    """Reject a moved/unknown published tag before release metadata is edited."""
    if not re.fullmatch(r'[0-9a-fA-F]{40}', sha):
        raise ValueError('A full source commit SHA is required')
    try:
        release = client.api('GET', 'releases/tags/' + quote(tag, safe=''))
    except subprocess.CalledProcessError as error:
        # Only a genuine missing release is allowed; auth/network failures block.
        if 'HTTP 404' in (error.stderr or ''):
            return None
        raise
    proofs = set(re.findall(r'https://github\.com/' + re.escape(client.repo)
                            + r'/tree/([0-9a-fA-F]{40})(?![0-9a-fA-F])', release.get('body') or ''))
    target = release.get('target_commitish', '')
    if re.fullmatch(r'[0-9a-fA-F]{40}', target):
        proofs.add(target)
    if {proof.lower() for proof in proofs} != {sha.lower()}:
        raise RuntimeError('Existing release source differs or cannot be certified; '
                           'retain the published tag and use a new version: ' + tag)
    return release


def prepare_draft(client, tag, sha, body, prerelease=False, attempts=5, sleep=time.sleep):
    """Create or reuse a Draft Release for (tag, sha) without exposing partial uploads."""
    existing_published = retry(lambda: verify_release_source(client, tag, sha), attempts, sleep)
    if existing_published is not None:
        return int(existing_published["id"])

    releases = retry(lambda: client.api("GET", "releases?per_page=100"), attempts, sleep) or []
    for rel in releases:
        if rel.get("draft") and rel.get("tag_name") == tag:
            target = (rel.get("target_commitish") or "").lower()
            if target != sha.lower():
                raise RuntimeError(
                    f"Existing draft release for {tag} targets {target or 'unknown'} instead of {sha}; "
                    "delete the stale draft before re-running."
                )
            rel_id = int(rel["id"])
            retry(
                lambda: client.api(
                    "PATCH",
                    f"releases/{rel_id}",
                    payload={"name": tag, "body": body, "prerelease": bool(prerelease)},
                ),
                attempts,
                sleep,
            )
            return rel_id

    created = retry(
        lambda: client.api(
            "POST",
            "releases",
            payload={
                "tag_name": tag,
                "target_commitish": sha,
                "name": tag,
                "body": body,
                "draft": True,
                "prerelease": bool(prerelease),
            },
        ),
        attempts,
        sleep,
    )
    return int(created["id"])


def publish(client, tag, path, release_id=None, attempts=5, sleep=time.sleep):
    with path.open("rb") as f:
        checksum = "sha256:" + hashlib.file_digest(f, "sha256").hexdigest()
    size = path.stat().st_size
    name = path.name
    pending_name = name + ".pending-" + checksum[7:23]

    def api(method, endpoint, payload=None, **fields):
        return retry(lambda: client.api(method, endpoint, payload=payload, **fields), attempts, sleep)

    def assets():
        if release_id is not None:
            release = api("GET", f"releases/{release_id}")
        else:
            release = api("GET", "releases/tags/" + quote(tag, safe=""))
        return release["assets"]

    def verified(asset):
        return (asset.get("state") == "uploaded" and asset.get("size") == size
                and asset.get("digest") == checksum)

    def existing_final():
        final = next((a for a in assets() if a["name"] == name), None)
        if final is not None and not verified(final):
            raise RuntimeError("Published asset differs or cannot be verified; retained unchanged. "
                               "Publish different bytes under a new version: " + name)
        return final

    final = existing_final()
    if final:
        print("Already published and SHA-256 verified:", name, flush=True)
        return final

    if release_id is not None:
        # Remove any stale incomplete pending upload with the same staging name before streaming.
        for stale in assets():
            if stale["name"] == pending_name:
                api("DELETE", f"releases/assets/{stale['id']}")
        retry(lambda: client.upload_by_id(release_id, pending_name, path), attempts, sleep)
    else:
        # Keep the package file unchanged; prefer a same-FS hardlink for the temporary staging name
        # (zero-copy for multi-hundred-MB packages) and fall back to copying when linking is unsupported.
        with tempfile.TemporaryDirectory(prefix="release-upload-", dir=str(path.parent)) as temporary:
            staged = Path(temporary) / pending_name
            try:
                os.link(path, staged)
            except OSError:
                shutil.copyfile(path, staged)
            retry(lambda: client.upload(tag, staged), attempts, sleep)

    pending = next((a for a in assets() if a["name"] == pending_name), None)
    if pending is None or not verified(pending):
        raise RuntimeError("Uploaded staging asset failed size/state/SHA-256 validation; "
                           "published assets remain unchanged")
    # A concurrent successful publisher wins. Never remove its published asset.
    final = existing_final()
    if final:
        api("DELETE", f"releases/assets/{pending['id']}")
        return final
    api("PATCH", f"releases/assets/{pending['id']}", payload={"name": name})
    final = existing_final()
    if final is None:
        raise RuntimeError("Renamed asset not visible; staging bytes retained for recovery")
    print("Published and SHA-256 verified:", name, flush=True)
    return final


def publish_draft(client, release_id, expected_assets, attempts=5, sleep=time.sleep):
    """Verify all expected assets are uploaded on release_id and flip draft=false atomically."""
    release = retry(lambda: client.api("GET", f"releases/{release_id}"), attempts, sleep)
    by_name = {a["name"]: a for a in release.get("assets") or []}
    missing = []
    for name in expected_assets:
        asset = by_name.get(name)
        if asset is None or asset.get("state") != "uploaded" or (asset.get("size") or 0) <= 0:
            missing.append(name)
    if missing:
        raise RuntimeError("Cannot publish draft release; missing or incomplete assets: " + ", ".join(missing))
    if release.get("draft"):
        release = retry(
            lambda: client.api("PATCH", f"releases/{release_id}", payload={"draft": False}),
            attempts,
            sleep,
        )
    print("Release live:", release.get("html_url") or release.get("tag_name") or release_id, flush=True)
    return release


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=os.environ.get("GH_REPO"), required=not os.environ.get("GH_REPO"))
    parser.add_argument("--tag", required=True)
    parser.add_argument("--release-id", type=int, default=None)
    parser.add_argument("--body-file", type=Path, default=None)
    parser.add_argument("--prerelease", action="store_true")
    parser.add_argument("--expect-asset", action="append", default=[])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--asset", type=Path)
    mode.add_argument("--verify-source", metavar="SHA")
    mode.add_argument("--prepare-draft", metavar="SHA")
    mode.add_argument("--publish-draft", type=int, metavar="RELEASE_ID")
    args = parser.parse_args()
    client = GitHub(args.repo)
    if args.verify_source:
        verify_release_source(client, args.tag, args.verify_source)
    elif args.prepare_draft:
        body = args.body_file.read_text(encoding="utf-8") if args.body_file else f"Release {args.tag}"
        rel_id = prepare_draft(client, args.tag, args.prepare_draft, body=body, prerelease=args.prerelease)
        print(rel_id)
    elif args.publish_draft is not None:
        publish_draft(client, args.publish_draft, args.expect_asset)
    else:
        publish(client, args.tag, args.asset, release_id=args.release_id)


if __name__ == "__main__":
    main()
