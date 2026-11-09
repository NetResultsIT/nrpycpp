"""Tests for tools/prepare_python_runtime.py (offline: network and artifactory calls are mocked).

Run from the repository root:  python3 -m unittest discover -s tests/tools -v
"""

import io
import json
import os
import sys
import tarfile
import tempfile
import unittest
import zipfile
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import prepare_python_runtime as prt  # noqa: E402


def touch(path, data=b"x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def make_tar(path, members):
    """members: list of (name, kind, payload) with kind file|dir|sym|lnk|fifo"""
    with tarfile.open(path, "w:gz") as tar:
        for name, kind, payload in members:
            info = tarfile.TarInfo(name)
            data = None
            if kind == "file":
                data = payload.encode()
                info.size = len(data)
            elif kind == "dir":
                info.type = tarfile.DIRTYPE
            elif kind == "sym":
                info.type = tarfile.SYMTYPE
                info.linkname = payload
            elif kind == "lnk":
                info.type = tarfile.LNKTYPE
                info.linkname = payload
            elif kind == "fifo":
                info.type = tarfile.FIFOTYPE
            tar.addfile(info, io.BytesIO(data) if data is not None else None)


class SafeExtractTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.archive = os.path.join(self.tmp.name, "a.tar.gz")
        self.dest = os.path.join(self.tmp.name, "out")
        os.makedirs(self.dest)

    def tearDown(self):
        self.tmp.cleanup()

    def test_regular_archive_is_extracted_without_share(self):
        make_tar(self.archive, [("python/lib/libpython3.13.so.1.0", "file", "elf"),
                                ("python/lib/libpython3.13.so", "sym", "libpython3.13.so.1.0"),
                                ("python/share/terminfo/n/nsterm", "file", "t")])
        prt.safe_extract(self.archive, self.dest)
        self.assertTrue(os.path.islink(os.path.join(self.dest, "python/lib/libpython3.13.so")))
        self.assertFalse(os.path.exists(os.path.join(self.dest, "python/share")))

    def test_rejects_unsafe_members(self):
        cases = {
            "parent": [("python/../../evil", "file", "x")],
            "absolute": [("/tmp/evil", "file", "x")],
            "symlink": [("python/lib/evil", "sym", "../../../etc/passwd")],
            "hardlink": [("python/evil", "lnk", "../outside")],
            "fifo": [("python/fifo", "fifo", None)],
        }
        for label, members in cases.items():
            with self.subTest(label):
                make_tar(self.archive, members)
                with self.assertRaises(SystemExit):
                    prt.safe_extract(self.archive, self.dest)


class PruneTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.tmp.name, "python")

    def tearDown(self):
        self.tmp.cleanup()

    def exists(self, rel):
        return os.path.lexists(os.path.join(self.root, rel))

    def test_unix_layout(self):
        std = "lib/python3.13/"
        for rel in ["bin/python3", "include/python3.13/Python.h", "lib/libpython3.13.dylib", "lib/libpython3.so",
                    "lib/libtcl9.0.dylib", "lib/tcl9.0/init.tcl", "lib/pkgconfig/python3.pc",
                    std + "encodings/__init__.py", std + "json/__init__.py", std + "tkinter/__init__.py",
                    std + "idlelib/idle.py", std + "ensurepip/__init__.py", std + "test/test_x.py",
                    std + "config-3.13-darwin/Makefile", std + "site-packages/pip/__init__.py",
                    std + "site-packages/README.txt", std + "lib-dynload/_dbm.cpython-313-darwin.so",
                    std + "lib-dynload/_testcapi.cpython-313-darwin.so", std + "json/__pycache__/x.pyc"]:
            touch(os.path.join(self.root, rel))
        prt.prune(self.root, "3.13.18", windows=False)
        for kept in ["include/python3.13/Python.h", "lib/libpython3.13.dylib", std + "encodings/__init__.py",
                     std + "json/__init__.py", std + "site-packages/README.txt"]:
            self.assertTrue(self.exists(kept), kept)
        for gone in ["bin", "lib/libpython3.so", "lib/libtcl9.0.dylib", "lib/tcl9.0", "lib/pkgconfig",
                     std + "tkinter", std + "idlelib", std + "ensurepip", std + "test", std + "config-3.13-darwin",
                     std + "site-packages/pip", std + "lib-dynload/_dbm.cpython-313-darwin.so",
                     std + "lib-dynload/_testcapi.cpython-313-darwin.so", std + "json/__pycache__"]:
            self.assertFalse(self.exists(gone), gone)

    def test_windows_layout(self):
        for rel in ["python.exe", "pythonw.exe", "python313.dll", "python3.dll", "vcruntime140.dll",
                    "include/Python.h", "libs/python313.lib", "Scripts/.empty", "tcl/tk86t.lib",
                    "DLLs/_ssl.pyd", "DLLs/libssl-3-x64.dll", "DLLs/_tkinter.pyd", "DLLs/tcl86t.dll",
                    "DLLs/_testcapi.pyd", "DLLs/winsound.pyd", "Lib/encodings/__init__.py",
                    "Lib/tkinter/__init__.py", "Lib/site-packages/pip/__init__.py"]:
            touch(os.path.join(self.root, rel))
        prt.prune(self.root, "3.13.18", windows=True)
        for kept in ["python313.dll", "python3.dll", "vcruntime140.dll", "include/Python.h", "libs/python313.lib",
                     "DLLs/_ssl.pyd", "DLLs/libssl-3-x64.dll", "Lib/encodings/__init__.py"]:
            self.assertTrue(self.exists(kept), kept)
        for gone in ["python.exe", "pythonw.exe", "Scripts", "tcl", "DLLs/_tkinter.pyd", "DLLs/tcl86t.dll",
                     "DLLs/_testcapi.pyd", "DLLs/winsound.pyd", "Lib/tkinter", "Lib/site-packages/pip"]:
            self.assertFalse(self.exists(gone), gone)

    def test_missing_encodings_is_fatal(self):
        touch(os.path.join(self.root, "Lib/site-packages/README.txt"))
        touch(os.path.join(self.root, "DLLs/_ssl.pyd"))
        with self.assertRaises(SystemExit):
            prt.prune(self.root, "3.13.18", windows=True)


class ResolveReleaseTest(unittest.TestCase):
    ASSETS = ["cpython-3.13.17+20261026-aarch64-apple-darwin-install_only_stripped.tar.gz",
              "cpython-3.13.17+20261026-x86_64-apple-darwin-install_only_stripped.tar.gz",
              "cpython-3.13.18+20261026-aarch64-apple-darwin-install_only_stripped.tar.gz",
              "cpython-3.13.18+20261026-x86_64-apple-darwin-install_only_stripped.tar.gz",
              "cpython-3.13.19+20261026-aarch64-apple-darwin-install_only_stripped.tar.gz",  # no x86_64
              "cpython-3.13.20+20261026-aarch64-apple-darwin-freethreaded-install_only_stripped.tar.gz",
              "cpython-3.14.1+20261026-aarch64-apple-darwin-install_only_stripped.tar.gz"]

    def release(self):
        return {"tag_name": "20261026", "assets": [{"name": n} for n in self.ASSETS]}

    def test_exact_version_needs_no_network(self):
        with mock.patch.object(prt, "fetch_json", side_effect=AssertionError("no network expected")):
            self.assertEqual(prt.resolve_release("3.13.18", "20261026", ["x"]), ("3.13.18", "20261026"))

    def test_newest_patch_available_for_every_target(self):
        with mock.patch.object(prt, "fetch_json", return_value=self.release()):
            targets = prt.PLATFORMS["macos"]["targets"]
            self.assertEqual(prt.resolve_release("3.13", None, targets), ("3.13.18", "20261026"))
            self.assertEqual(prt.resolve_release("3.13", None, targets[:1]), ("3.13.19", "20261026"))

    def test_unknown_version_is_fatal(self):
        with mock.patch.object(prt, "fetch_json", return_value=self.release()):
            with self.assertRaises(SystemExit):
                prt.resolve_release("3.12", None, ["aarch64-apple-darwin"])


class PackageTest(unittest.TestCase):
    def test_zip_and_tgz_have_a_single_python_top_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            touch(os.path.join(tmp, "python/Lib/encodings/__init__.py"))
            touch(os.path.join(tmp, "python/python313.dll"))
            prt.make_package(tmp, os.path.join(tmp, "p.zip"))
            prt.make_package(tmp, os.path.join(tmp, "p.tgz"))
            with zipfile.ZipFile(os.path.join(tmp, "p.zip")) as z:
                names = z.namelist()
            with tarfile.open(os.path.join(tmp, "p.tgz")) as t:
                names_tgz = t.getnames()
            self.assertEqual(sorted(names), ["python/Lib/encodings/__init__.py", "python/python313.dll"])
            self.assertEqual({n.split("/")[0] for n in names_tgz}, {"python"})
            self.assertIn("python/Lib/encodings/__init__.py", names_tgz)


class UploadTest(unittest.TestCase):
    """upload must never overwrite: it checks every path first and verifies what it sent."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = self.tmp.name
        self.rel = "deps/python/release/3.13.18/win64/python-3.13.18-win64_vs2019.zip"
        touch(os.path.join(self.out, "upload", self.rel), b"package")
        self.sha1 = prt.file_digest(os.path.join(self.out, "upload", self.rel), "sha1")
        key = os.path.join(self.out, "key.txt")
        touch(key, b"secret\n")
        self.patches = [mock.patch.object(prt, "TOKEN_FILE", key), mock.patch.object(prt, "READ_KEY_FILE", key)]
        for p in self.patches:
            p.start()

    def tearDown(self):
        for p in self.patches:
            p.stop()
        self.tmp.cleanup()

    def args(self, dry_run=False):
        return mock.Mock(out=self.out, dry_run=dry_run)

    def test_existing_path_with_other_content_is_never_overwritten(self):
        calls = []

        def http(method, url, auth, data=None, headers=None):
            calls.append(method)
            return 200, json.dumps({"checksums": {"sha1": "0" * 40}, "size": 1}).encode()
        with mock.patch.object(prt, "http", side_effect=http):
            with self.assertRaises(SystemExit):
                prt.upload(self.args())
        self.assertNotIn("PUT", calls)

    def test_identical_existing_file_is_skipped(self):
        calls = []

        def http(method, url, auth, data=None, headers=None):
            calls.append(method)
            return 200, json.dumps({"checksums": {"sha1": self.sha1}, "size": 7}).encode()
        with mock.patch.object(prt, "http", side_effect=http):
            prt.upload(self.args())
        self.assertEqual(calls, ["GET"])

    def test_dry_run_does_not_upload_and_uses_the_read_key(self):
        seen = []

        def http(method, url, auth, data=None, headers=None):
            seen.append((method, auth))
            return 404, b""
        with mock.patch.object(prt, "http", side_effect=http):
            prt.upload(self.args(dry_run=True))
        self.assertEqual([m for m, _ in seen], ["GET"])
        self.assertEqual(seen[0][1], {"X-JFrog-Art-Api": "secret"})

    def test_upload_sends_checksum_and_verifies(self):
        sent = {}
        state = {"uploaded": False}

        def http(method, url, auth, data=None, headers=None):
            if method == "PUT":
                sent.update(url=url, auth=auth, headers=headers, data=data)
                state["uploaded"] = True
                return 201, b"{}"
            if not state["uploaded"]:
                return 404, b""
            return 200, json.dumps({"checksums": {"sha1": self.sha1}, "size": "7"}).encode()
        with mock.patch.object(prt, "http", side_effect=http):
            prt.upload(self.args())
        self.assertTrue(sent["url"].endswith("/bblocks/" + self.rel))
        self.assertEqual(sent["auth"], {"Authorization": "Bearer secret"})
        self.assertEqual(sent["headers"]["X-Checksum-Sha1"], self.sha1)
        self.assertEqual(sent["data"], b"package")

    def test_failed_verification_is_fatal(self):
        def http(method, url, auth, data=None, headers=None):
            if method == "PUT":
                return 201, b"{}"
            return (404, b"") if not getattr(http, "put", False) else (200, b'{"checksums": {"sha1": "bad"}}')

        def side_effect(method, *a, **k):
            if method == "PUT":
                http.put = True
            return http(method, *a, **k)
        with mock.patch.object(prt, "http", side_effect=side_effect):
            with self.assertRaises(SystemExit):
                prt.upload(self.args())


if __name__ == "__main__":
    unittest.main()
