import hashlib
import json
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from io import BytesIO, StringIO
from pathlib import Path
from unittest import mock

from . import prepare


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def required_fingerprint(required_files, payload) -> str:
    digest = hashlib.sha256()
    for relative in required_files:
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(sha256_bytes(payload[relative]).encode("ascii"))
        digest.update(b"\0")
    return digest.hexdigest()


class PrepareWebRtcTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        prepare.deps_dir = self.root / "deps"
        prepare.cache_keys_dir = self.root / "cache"
        prepare.cache_keys_dir.mkdir(parents=True)
        prepare._validated_webrtc_archives.clear()

        self.required_files = (
            "include/api/peer_connection_interface.h",
            "lib/Release/webrtc.lib",
            "lib/Debug/webrtc.lib",
            "include/boringssl-prefix/Release/boringssl_prefix_symbols.h",
            "include/boringssl-prefix/Debug/boringssl_prefix_symbols.h",
        )
        self.payload = {
            relative: f"content:{relative}".encode("utf-8")
            for relative in self.required_files
        }
        self.payload["lib/Release/webrtc.lib"] = b"release-library"
        self.payload["lib/Debug/webrtc.lib"] = b"debug-library"
        self.archive = self.root / "input.zip"
        self._create_archive(self.archive)

    def tearDown(self):
        self.temporary.cleanup()

    def _create_archive(self, path: Path):
        prefix = "test-webrtc"
        required_hash = required_fingerprint(self.required_files, self.payload)
        configurations = {
            "Debug": {"library_sha256": sha256_bytes(
                self.payload["lib/Debug/webrtc.lib"])},
            "Release": {"library_sha256": sha256_bytes(
                self.payload["lib/Release/webrtc.lib"])},
        }
        source = {"webrtc": "a" * 40, "build": "b" * 40,
                  "boringssl": "c" * 40}
        manifest = {
            "schema_version": 1,
            "package_id": "test-dual-config",
            "archive_prefix": prefix,
            "boringssl_prefix": "cohavora_bssl",
            "source": source,
            "configurations": configurations,
            "required_files": list(self.required_files),
            "required_files_sha256": required_hash,
        }
        manifest_bytes = (
            json.dumps(manifest, sort_keys=True) + "\n").encode("utf-8")
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for relative, content in self.payload.items():
                archive.writestr(f"{prefix}/{relative}", content)
            archive.writestr(f"{prefix}/metadata/package.json", manifest_bytes)

        metadata = {
            **manifest,
            "archive": "test-dual-config.zip",
            "size": path.stat().st_size,
            "sha256": prepare.compute_file_hash(path),
            "file_count": len(self.payload) + 1,
            "manifest_sha256": sha256_bytes(manifest_bytes),
        }
        prepare.WEBRTC_PACKAGE_METADATA = metadata
        prepare.WEBRTC_PACKAGE_ARCHIVE = metadata["archive"]
        prepare.WEBRTC_PACKAGE_URL = "https://example.invalid/test-dual-config.zip"
        prepare.WEBRTC_PACKAGE_SHA256 = metadata["sha256"]
        prepare.WEBRTC_PACKAGE_SIZE = metadata["size"]
        prepare.WEBRTC_PACKAGE_CACHE_ID = (
            f"test:{metadata['package_id']}:{metadata['sha256']}")
        prepare.WEBRTC_REQUIRED_FILES = self.required_files

    def test_installs_atomically_and_reuses_verified_cache(self):
        target = prepare.prepare_webrtc(self.archive)
        self.assertEqual(
            (target / "lib/Release/webrtc.lib").read_bytes(),
            b"release-library")
        self.assertEqual(
            (target / "lib/Debug/webrtc.lib").read_bytes(),
            b"debug-library")
        self.assertNotEqual(
            (target / "lib/Release/webrtc.lib").read_bytes(),
            (target / "lib/Debug/webrtc.lib").read_bytes())

        with mock.patch.object(
                prepare, "safe_extract_zip",
                side_effect=AssertionError("cache hit extracted the archive")):
            output = StringIO()
            with redirect_stdout(output):
                self.assertEqual(prepare.prepare_webrtc(), target)
        self.assertIn("verified dual-config prefixed SDK", output.getvalue())

    def test_rejects_archive_hash_mismatch(self):
        damaged = self.root / "damaged.zip"
        content = bytearray(self.archive.read_bytes())
        content[len(content) // 2] ^= 1
        damaged.write_bytes(content)
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            prepare.validate_webrtc_archive(damaged)

    def test_downloads_missing_pinned_archive(self):
        response = BytesIO(self.archive.read_bytes())
        with mock.patch.object(
                prepare.urllib.request, "urlopen", return_value=response) as urlopen:
            cached = prepare.resolve_webrtc_archive()

        self.assertEqual(
            cached, prepare.cache_keys_dir / prepare.WEBRTC_PACKAGE_ARCHIVE)
        self.assertEqual(cached.read_bytes(), self.archive.read_bytes())
        request = urlopen.call_args.args[0]
        self.assertEqual(request.full_url, prepare.WEBRTC_PACKAGE_URL)

    def test_repairs_incomplete_install_from_cached_package(self):
        target = prepare.prepare_webrtc(self.archive)
        missing = target / "lib/Debug/webrtc.lib"
        missing.unlink()
        self.assertFalse(prepare.webrtc_install_is_valid(target))

        prepare.prepare_webrtc()
        self.assertEqual(missing.read_bytes(), b"debug-library")
        self.assertTrue(prepare.webrtc_install_is_valid(target))

    def test_atomic_replace_restores_old_tree_when_swap_fails(self):
        target = prepare.deps_dir / "webrtc"
        staged = self.root / "staged"
        target.mkdir(parents=True)
        staged.mkdir()
        (target / "marker.txt").write_text("old", encoding="ascii")
        (staged / "marker.txt").write_text("new", encoding="ascii")
        original_rename = Path.rename

        def fail_staged_rename(path, destination):
            if path == staged:
                raise OSError("injected replacement failure")
            return original_rename(path, destination)

        with mock.patch.object(Path, "rename", new=fail_staged_rename):
            with self.assertRaisesRegex(OSError, "injected replacement failure"):
                prepare.atomic_replace_directory(staged, target)

        self.assertEqual(
            (target / "marker.txt").read_text(encoding="ascii"), "old")


if __name__ == "__main__":
    unittest.main()
