"""SDK integrity checks using tiny fixtures, independent of a WebRTC checkout."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('sdk', Path(__file__).parents[1] / 'scripts/webrtc-sdk.py')
sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sdk)


class SdkIntegrityTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        files = {}
        for name in ('obj/webrtc.lib', 'args.gn', 'include/api/peer_connection_interface.h', 'notices/LICENSE.md'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name)
            files[name] = sdk.digest(path)
        self.manifest = {'schemaVersion': 2, 'files': files}
        self.save()

    def save(self):
        self.manifest.pop('cacheIdentity', None)
        self.manifest['cacheIdentity'] = hashlib.sha256(sdk.canonical(self.manifest)).hexdigest()
        (self.root / 'screenshare-artifact.json').write_text(json.dumps(self.manifest))

    def test_valid(self):
        sdk.verify(self.root)

    def test_changed_header(self):
        (self.root / 'include/api/peer_connection_interface.h').write_text('tampered')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            sdk.verify(self.root)

    def test_unlisted_header(self):
        (self.root / 'include/injected.h').write_text('extra')
        with self.assertRaisesRegex(ValueError, 'inventory'):
            sdk.verify(self.root)

    def test_wrong_identity(self):
        path = self.root / 'screenshare-artifact.json'
        manifest = dict(self.manifest, cacheIdentity='0' * 64)
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'identity'):
            sdk.verify(self.root)

    def test_outside_path(self):
        with self.assertRaisesRegex(ValueError, 'relative path'):
            sdk.checked_path(self.root, '../outside')


if __name__ == '__main__':
    unittest.main()
