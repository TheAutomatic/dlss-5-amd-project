"""Upload a release asset with retries, without deleting a published asset.

An existing same-name asset must have the same SHA-256. Different bytes require
a new release version. Incomplete uploads use a temporary name, so an interrupted
transfer cannot remove the download users already have.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from urllib.parse import quote


class GitHub:
    def __init__(self, repo):
        self.repo = repo

    def api(self, method, path, **fields):
        command = ["gh", "api", "--method", method, f"repos/{self.repo}/{path}"]
        for key, value in fields.items():
            command.extend(["-f", f"{key}={value}"])
        result = subprocess.run(command, check=True, capture_output=True, text=True, encoding="utf-8", timeout=60)
        return json.loads(result.stdout) if result.stdout.strip() else None

    def upload(self, tag, path):
        # Only the private temporary name is replaceable, never the published name.
        subprocess.run(["gh", "release", "upload", tag, str(path), "--repo", self.repo,
                        "--clobber"], check=True, timeout=180)


def retry(operation, attempts, sleep):
    for attempt in range(attempts):
        try:
            return operation()
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError):
            if attempt + 1 == attempts:
                raise
            delay = min(30, 2 ** (attempt + 1))
            print(f"GitHub transfer/API failed; retry {attempt + 2}/{attempts} in {delay}s", flush=True)
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
            return
        raise
    proofs = set(re.findall(r'https://github\.com/' + re.escape(client.repo)
                            + r'/tree/([0-9a-fA-F]{40})(?![0-9a-fA-F])', release.get('body') or ''))
    target = release.get('target_commitish', '')
    if re.fullmatch(r'[0-9a-fA-F]{40}', target):
        proofs.add(target)
    if {proof.lower() for proof in proofs} != {sha.lower()}:
        raise RuntimeError('Existing release source differs or cannot be certified; '
                           'retain the published tag and use a new version: ' + tag)


def publish(client, tag, path, attempts=5, sleep=time.sleep):
    with path.open("rb") as f:
        checksum = "sha256:" + hashlib.file_digest(f, "sha256").hexdigest()
    size = path.stat().st_size
    name = path.name
    pending_name = name + ".pending-" + checksum[7:23]

    def api(method, endpoint, **fields):
        return retry(lambda: client.api(method, endpoint, **fields), attempts, sleep)

    def assets():
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
        print("Already published and SHA-256 verified:", name)
        return final
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
    api("PATCH", f"releases/assets/{pending['id']}", name=name)
    final = existing_final()
    if final is None:
        raise RuntimeError("Renamed asset not visible; staging bytes retained for recovery")
    print("Published and SHA-256 verified:", name)
    return final


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=os.environ.get("GH_REPO"), required=not os.environ.get("GH_REPO"))
    parser.add_argument("--tag", required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--asset", type=Path)
    mode.add_argument("--verify-source", metavar="SHA")
    args = parser.parse_args()
    client = GitHub(args.repo)
    if args.verify_source:
        verify_release_source(client, args.tag, args.verify_source)
    else:
        publish(client, args.tag, args.asset)


if __name__ == "__main__":
    main()
