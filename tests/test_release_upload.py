import hashlib
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("publisher", Path(__file__).resolve().parents[1] / ".github/scripts/publish_release_asset.py")
publisher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publisher)


class FakeGitHub:
    def __init__(self):
        self.assets = []
        self.upload_failures = 0
        self.upload_calls = 0
        self.bad_digest = False
        self.rename_disconnect = False
        self.deletes = []

    def api(self, method, path, **fields):
        if method == "GET":
            return {"assets": [dict(a) for a in self.assets]}
        item = next(a for a in self.assets if str(a["id"]) == path.rsplit("/", 1)[-1])
        if method == "PATCH":
            item.update(fields)
            if self.rename_disconnect:
                self.rename_disconnect = False
                raise subprocess.CalledProcessError(1, ["gh"])
            return dict(item)
        if method == "DELETE":
            self.deletes.append(item["name"])
            self.assets.remove(item)

    def upload(self, tag, path):
        self.upload_calls += 1
        if self.upload_failures:
            self.upload_failures -= 1
            raise subprocess.CalledProcessError(1, ["gh"])
        data = path.read_bytes()
        self.assets = [a for a in self.assets if a["name"] != path.name]
        self.assets.append({"id": 42, "name": path.name, "state": "uploaded", "size": len(data),
                            "digest": "wrong" if self.bad_digest else "sha256:" + hashlib.sha256(data).hexdigest()})


class UploadTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "release.zip"
        self.path.write_bytes(b"package")
        self.client = FakeGitHub()

    def publish(self):
        return publisher.publish(self.client, "v1", self.path, attempts=3, sleep=lambda _: None)

    def test_retries_upload_then_verifies_and_renames(self):
        self.client.upload_failures = 2
        result = self.publish()
        self.assertEqual(self.client.upload_calls, 3)
        self.assertEqual(result["name"], self.path.name)
        self.assertFalse(self.client.deletes)

    def test_rerun_is_idempotent(self):
        self.publish(); self.publish()
        self.assertEqual(self.client.upload_calls, 1)

    def test_different_published_asset_is_never_deleted(self):
        self.client.assets = [{"id": 1, "name": "release.zip", "size": 1, "state": "uploaded", "digest": "old"}]
        with self.assertRaises(RuntimeError): self.publish()
        self.assertFalse(self.client.upload_calls)
        self.assertEqual(self.client.assets[0]["digest"], "old")
        self.assertFalse(self.client.deletes)

    def test_failed_upload_does_not_publish(self):
        self.client.upload_failures = 5
        with self.assertRaises(subprocess.CalledProcessError): self.publish()
        self.assertFalse(self.client.assets)

    def test_corrupt_upload_stays_temporary(self):
        self.client.bad_digest = True
        with self.assertRaises(RuntimeError): self.publish()
        self.assertIn(".pending-", self.client.assets[0]["name"])

    def test_ambiguous_rename_retries_without_losing_asset(self):
        self.client.rename_disconnect = True
        self.assertEqual(self.publish()["name"], "release.zip")
        self.assertEqual(len(self.client.assets), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
