"""Shipping HIP binaries must survive Git's clean and checkout filters."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def git(root, *args):
    return subprocess.run(['git', '-C', str(root), *args], check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


class BinaryAttributesTests(unittest.TestCase):
    def test_shipping_modules_round_trip(self):
        modules = sorted((ROOT / 'third_party/lmxxf/modules').rglob('*.hsaco'))
        self.assertTrue(modules, 'No shipping modules found')
        scratch = ROOT / 'work/scratch'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='module-git-', dir=scratch) as temp:
            tree = Path(temp).resolve()
            self.assertEqual(tree.parent, scratch.resolve())
            self.assertFalse(tree.is_symlink())
            git(tree, 'init', '-q')
            git(tree, 'config', 'core.autocrlf', 'true')
            shutil.copyfile(ROOT / '.gitattributes', tree / '.gitattributes')
            expected = {}
            for source in modules:
                relative = source.relative_to(ROOT)
                target = tree / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
                expected[relative] = hashlib.sha256(source.read_bytes()).digest()
                raw = git(tree, 'hash-object', '--no-filters', relative.as_posix())
                filtered = git(tree, 'hash-object', '--path=' + relative.as_posix(), relative.as_posix())
                self.assertEqual(raw, filtered, f'Git changes binary bytes: {relative}')
            git(tree, 'add', '.gitattributes', 'third_party/lmxxf/modules')
            git(tree, '-c', 'user.name=Module test', '-c', 'user.email=test@example.invalid',
                '-c', 'core.hooksPath=', 'commit', '-qm', 'binary fixture')
            for relative in expected:
                (tree / relative).unlink()
            git(tree, 'checkout', '--', 'third_party/lmxxf/modules')
            for relative, digest in expected.items():
                self.assertEqual(hashlib.sha256((tree / relative).read_bytes()).digest(), digest,
                                 f'Checkout changed module: {relative}')
            self.assertEqual(git(tree, 'status', '--porcelain'), b'')


if __name__ == '__main__':
    unittest.main()
