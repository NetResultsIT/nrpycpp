#!/usr/bin/env python3
"""Prepare (and publish) the CPython runtime embedded by nrPyCpp.

The runtime comes from python-build-standalone (https://github.com/astral-sh/python-build-standalone),
relocatable CPython builds with headers and libraries. For each VDK platform this script downloads the
"install_only_stripped" archive, verifies it against the release SHA256SUMS, prunes what an embedded,
headless interpreter does not need and packages it with the VDK dependencies conventions:

  platform               upstream target(s)                          artifactory path (repo bblocks)
  macos                  aarch64-apple-darwin + x86_64-apple-darwin  deps/python/release/<ver>/macos/python-<ver>-macos.tgz
                         (merged into universal binaries)
  linux64_Ubuntu_22.04   x86_64-unknown-linux-gnu (glibc >= 2.17)    deps/python/release/<ver>/linux64/python-<ver>-linux64_Ubuntu_22.04.tgz
  win64_vs2019           x86_64-pc-windows-msvc                      deps/python/release/<ver>/win64/python-<ver>-win64_vs2019.zip

Every package has a single top folder "python/" holding the runtime, the headers (and the import
libraries on windows) and NRPYCPP_PYTHON.json, which records where it comes from.

Commands:
  prepare   download, verify, prune, package       (into <out>/upload/deps/python/...)
  upload    publish <out>/upload on artifactory    (never overwrites, needs ~/.nr/arti_adm_token.txt)

Examples:
  tools/prepare_python_runtime.py prepare --platform macos --out build/python            # latest 3.13.x
  tools/prepare_python_runtime.py prepare --platform win64_vs2019 --python 3.13.18 --release 20261026 --out build/python
  tools/prepare_python_runtime.py upload --out build/python --dry-run

Only the python standard library is used (python >= 3.8); the macos platform needs lipo and codesign.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.error
import urllib.request
import zipfile

GITHUB_API = "https://api.github.com/repos/astral-sh/python-build-standalone/releases"
DOWNLOAD_URL = "https://github.com/astral-sh/python-build-standalone/releases/download/{release}/{name}"
FLAVOR = "install_only_stripped"
ARTIFACTORY = "https://nrreleases.jfrog.io/artifactory"
REPO = "bblocks"
MODULE = "deps/python"
TOKEN_FILE = os.path.expanduser("~/.nr/arti_adm_token.txt")       # upload rights (Bearer)
READ_KEY_FILE = os.path.expanduser("~/.nr/artifactory_api_key")  # read only, enough for --dry-run

PLATFORMS = {
    "macos": {"targets": ["aarch64-apple-darwin", "x86_64-apple-darwin"], "folder": "macos", "ext": "tgz"},
    "linux64_Ubuntu_22.04": {"targets": ["x86_64-unknown-linux-gnu"], "folder": "linux64", "ext": "tgz"},
    "win64_vs2019": {"targets": ["x86_64-pc-windows-msvc"], "folder": "win64", "ext": "zip"},
}

# stdlib packages that are of no use in an embedded, headless interpreter
STDLIB_PRUNE = ("tkinter", "idlelib", "turtledemo", "ensurepip", "pydoc_data", "test", "turtle.py")
# extension modules that go with the pruned packages, are only CPython's own tests, or are dbm backends
# (dbm.ndbm/gnu: the only modules outside libpython on unix, dbm falls back to its pure python backends)
EXTENSION_PRUNE = ("_test", "_ctypes_test", "_tkinter", "xxlimited", "xxsubtype", "_xxtestfuzz", "_dbm", "_gdbm")


def log(msg):
    print(msg, flush=True)


def fail(msg):
    sys.exit("ERROR: " + msg)


# ---------------------------------------------------------------------------------------------- download

def fetch_json(url):
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req) as resp:
        return json.load(resp)


def asset_name(version, release, target):
    return "cpython-{}+{}-{}-{}.tar.gz".format(version, release, target, FLAVOR)


def resolve_release(python, release, targets):
    """Return (full_version, release), resolving 'latest' and X.Y versions through the github API."""
    if release and re.fullmatch(r"\d+\.\d+\.\d+", python):
        return python, release
    data = fetch_json(GITHUB_API + ("/latest" if not release else "/tags/" + release))
    names = {a["name"] for a in data.get("assets", [])}
    pattern = re.compile(r"cpython-({}(?:\.\d+)?)\+(\d+)-{}-{}\.tar\.gz$".format(
        re.escape(python), re.escape(targets[0]), FLAVOR))
    found = []
    for name in names:
        m = pattern.match(name)
        if m and all(asset_name(m.group(1), m.group(2), t) in names for t in targets):
            found.append((tuple(int(x) for x in m.group(1).split(".")), m.group(1), m.group(2)))
    if not found:
        fail("no {} build of python {} for {} in release {}".format(FLAVOR, python, targets, data.get("tag_name")))
    found.sort()
    return found[-1][1], found[-1][2]


def release_checksums(release):
    with urllib.request.urlopen(DOWNLOAD_URL.format(release=release, name="SHA256SUMS")) as resp:
        sums = {}
        for line in resp.read().decode().splitlines():
            parts = line.split()
            if len(parts) == 2:
                sums[parts[1]] = parts[0].lower()
        return sums


def file_digest(path, algorithm="sha256"):
    h = hashlib.new(algorithm)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url, dest):
    log("downloading " + url)
    tmp = dest + ".part"
    with urllib.request.urlopen(url) as resp, open(tmp, "wb") as f:
        shutil.copyfileobj(resp, f)
    os.replace(tmp, dest)


def safe_extract(archive, dest):
    """Extract refusing absolute paths, '..', links pointing outside of dest and special files."""
    dest = os.path.realpath(dest)
    with tarfile.open(archive, "r:gz") as tar:
        # share/ only holds man pages and terminfo, useless when embedding, and terminfo has names
        # differing only by case that cannot be extracted on case-insensitive filesystems (macOS, windows)
        members = [m for m in tar.getmembers() if not m.name.startswith("python/share/")]
        for member in members:
            target = os.path.realpath(os.path.join(dest, member.name))
            if not target.startswith(dest + os.sep):
                fail("refusing to extract {}: outside of the destination".format(member.name))
            if member.issym() or member.islnk():
                base = dest if member.islnk() else os.path.dirname(target)
                link = os.path.realpath(os.path.join(base, member.linkname))
                if not link.startswith(dest + os.sep):
                    fail("refusing to extract {}: link outside of the destination".format(member.name))
            elif not (member.isfile() or member.isdir()):
                fail("refusing to extract {}: unsupported member type".format(member.name))
        if sys.version_info >= (3, 12):
            tar.extractall(dest, members=members, filter="tar")
        else:
            tar.extractall(dest, members=members)


# ---------------------------------------------------------------------------------------------- prune

def remove(path):
    if os.path.islink(path) or os.path.isfile(path):
        os.remove(path)
    elif os.path.isdir(path):
        shutil.rmtree(path)


def stdlib_dir(root, version, windows):
    minor = ".".join(version.split(".")[:2])
    return os.path.join(root, "Lib") if windows else os.path.join(root, "lib", "python" + minor)


def prune(root, version, windows):
    """Remove everything that is not needed to build against and embed the interpreter."""
    stdlib = stdlib_dir(root, version, windows)
    if windows:
        for name in ("Scripts", "tcl", "python.exe", "pythonw.exe"):
            remove(os.path.join(root, name))
        dlls = os.path.join(root, "DLLs")
        for name in os.listdir(dlls):
            if name.startswith(EXTENSION_PRUNE + ("tcl", "tk")) or name == "winsound.pyd":
                remove(os.path.join(dlls, name))
    else:
        remove(os.path.join(root, "bin"))
        remove(os.path.join(root, "share"))
        lib = os.path.join(root, "lib")
        for name in os.listdir(lib):
            if name.startswith(("tcl", "tk", "itcl", "thread", "libtcl", "libtk", "pkgconfig")) or name == "libpython3.so":
                remove(os.path.join(lib, name))
        for name in os.listdir(stdlib):
            if name.startswith("config-"):
                remove(os.path.join(stdlib, name))
        dynload = os.path.join(stdlib, "lib-dynload")
        for name in os.listdir(dynload):
            if name.startswith(EXTENSION_PRUNE):
                remove(os.path.join(dynload, name))
    for name in STDLIB_PRUNE:
        remove(os.path.join(stdlib, name))
    site_packages = os.path.join(stdlib, "site-packages")
    for name in os.listdir(site_packages):
        if name != "README.txt":
            remove(os.path.join(site_packages, name))
    for dirpath, dirnames, _ in os.walk(root):
        for d in list(dirnames):
            if d == "__pycache__":
                shutil.rmtree(os.path.join(dirpath, d))
                dirnames.remove(d)
    if not os.path.isfile(os.path.join(stdlib, "encodings", "__init__.py")):
        fail("pruning went wrong: the encodings package is missing")


# ---------------------------------------------------------------------------------------------- macos universal

def run(cmd):
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True).stdout


def is_macho(path):
    with open(path, "rb") as f:
        return f.read(4) in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xfe\xed\xfa\xcf")


def make_universal(arm64_root, x86_64_root, version):
    """Merge the x86_64 tree into the arm64 one: lipo the binaries, dispatch pyconfig.h by architecture."""
    for dirpath, _, files in os.walk(arm64_root):
        for f in files:
            path = os.path.join(dirpath, f)
            if os.path.islink(path) or not is_macho(path):
                continue
            other = os.path.join(x86_64_root, os.path.relpath(path, arm64_root))
            if not os.path.isfile(other):
                fail("no x86_64 counterpart for " + path)
            run(["lipo", "-create", path, other, "-output", path + ".universal"])
            os.replace(path + ".universal", path)
            run(["codesign", "--force", "--sign", "-", path])
    minor = ".".join(version.split(".")[:2])
    include = os.path.join(arm64_root, "include", "python" + minor)
    shutil.move(os.path.join(include, "pyconfig.h"), os.path.join(include, "pyconfig-arm64.h"))
    shutil.copy2(os.path.join(x86_64_root, "include", "python" + minor, "pyconfig.h"),
                 os.path.join(include, "pyconfig-x86_64.h"))
    with open(os.path.join(include, "pyconfig.h"), "w") as f:
        f.write("/* universal (arm64 + x86_64) build: the configuration differs by architecture */\n"
                "#if defined(__arm64__) || defined(__aarch64__)\n#include \"pyconfig-arm64.h\"\n"
                "#elif defined(__x86_64__)\n#include \"pyconfig-x86_64.h\"\n"
                "#else\n#error \"unsupported architecture\"\n#endif\n")


def verify_macos(root):
    for dirpath, _, files in os.walk(root):
        for f in files:
            path = os.path.join(dirpath, f)
            if os.path.islink(path) or not is_macho(path):
                continue
            archs = run(["lipo", "-archs", path]).split()
            if sorted(archs) != ["arm64", "x86_64"]:
                fail("{} is not universal: {}".format(path, archs))
            if "LC_RPATH" in run(["otool", "-l", path]):
                fail("{} has an LC_RPATH".format(path))
            run(["codesign", "--verify", path])
            log("  ok {} ({})".format(os.path.relpath(path, root), " ".join(archs)))


# ---------------------------------------------------------------------------------------------- package

def tree_size(root):
    total = 0
    for dirpath, _, files in os.walk(root):
        for f in files:
            p = os.path.join(dirpath, f)
            if not os.path.islink(p):
                total += os.path.getsize(p)
    return total


def make_package(parent, out_file):
    """Package <parent>/python as out_file, with a deterministic file order."""
    root = os.path.join(parent, "python")
    entries = []
    for dirpath, dirnames, files in os.walk(root):
        dirnames.sort()
        for name in sorted(files):
            entries.append(os.path.join(dirpath, name))
    if out_file.endswith(".zip"):
        with zipfile.ZipFile(out_file, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for path in entries:
                z.write(path, os.path.relpath(path, parent))
    else:
        with tarfile.open(out_file, "w:gz", compresslevel=9, format=tarfile.PAX_FORMAT) as tar:
            tar.add(root, arcname="python", recursive=False)
            for path in entries:
                tar.add(path, arcname=os.path.relpath(path, parent), recursive=False)


def prepare(args):
    spec = PLATFORMS[args.platform]
    targets = spec["targets"]
    version, release = resolve_release(args.python, args.release, targets)
    log("python {} (python-build-standalone {}) for {}".format(version, release, args.platform))

    out = os.path.abspath(args.out)
    downloads = os.path.join(out, "downloads")
    work = os.path.join(out, "work", args.platform)
    os.makedirs(downloads, exist_ok=True)
    remove(work)
    sums = release_checksums(release)

    roots = []
    upstream = []
    for target in targets:
        name = asset_name(version, release, target)
        archive = os.path.join(downloads, name)
        if not os.path.isfile(archive):
            download(DOWNLOAD_URL.format(release=release, name=name.replace("+", "%2B")), archive)
        digest = file_digest(archive)
        if name not in sums or sums[name] != digest:
            fail("checksum mismatch for {}: expected {}, got {}".format(name, sums.get(name), digest))
        log("sha256 ok {}".format(name))
        dest = os.path.join(work, target)
        os.makedirs(dest)
        safe_extract(archive, dest)
        prune(os.path.join(dest, "python"), version, "windows" in target)
        roots.append(os.path.join(dest, "python"))
        upstream.append({"archive": name, "sha256": digest})

    if args.platform == "macos":
        make_universal(roots[0], roots[1], version)
        verify_macos(roots[0])
    root = roots[0]
    stdlib = stdlib_dir(root, version, "win" in args.platform)
    if not os.path.isfile(os.path.join(stdlib, "encodings", "__init__.py")):
        fail("the stdlib is missing from the package")

    with open(os.path.join(root, "NRPYCPP_PYTHON.json"), "w") as f:
        json.dump({"python_version": version, "pbs_release": release, "platform": args.platform,
                   "flavor": FLAVOR, "upstream": upstream,
                   "prepared_by": "nrpycpp tools/prepare_python_runtime.py"}, f, indent=2)
        f.write("\n")

    rel_dir = "{}/release/{}/{}".format(MODULE, version, spec["folder"])
    file_name = "python-{}-{}{}.{}".format(version, args.platform, "-{}".format(args.revision) if args.revision else "",
                                           spec["ext"])
    dist = os.path.join(out, "upload", rel_dir)
    os.makedirs(dist, exist_ok=True)
    package = os.path.join(dist, file_name)
    make_package(os.path.dirname(root), package)
    log("runtime {:5.1f} MB on disk, package {:5.1f} MB: {}".format(
        tree_size(root) / 2**20, os.path.getsize(package) / 2**20, package))


# ---------------------------------------------------------------------------------------------- upload

def http(method, url, auth, data=None, headers=None):
    req = urllib.request.Request(url, data=data, method=method, headers=dict(headers or {}))
    for k, v in auth.items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def upload(args):
    base = os.path.join(os.path.abspath(args.out), "upload")
    files = []
    for dirpath, _, names in os.walk(base):
        files += [os.path.join(dirpath, n) for n in names if not n.startswith(".")]
    if not files:
        fail("nothing to upload in " + base)
    key_file = READ_KEY_FILE if args.dry_run else TOKEN_FILE
    if not os.path.isfile(key_file):
        fail("{} not found: the packages are in {}".format(key_file, base))
    with open(key_file) as f:
        key = f.read().strip()
    auth = {"X-JFrog-Art-Api": key} if args.dry_run else {"Authorization": "Bearer " + key}
    for path in sorted(files):
        rel = os.path.relpath(path, base).replace(os.sep, "/")
        sha1 = file_digest(path, "sha1")
        status, body = http("GET", "{}/api/storage/{}/{}".format(ARTIFACTORY, REPO, rel), auth)
        if status == 200:
            if json.loads(body).get("checksums", {}).get("sha1") == sha1:
                log("already published (same sha1): " + rel)
                continue
            fail("{} is already published with different content: never overwritten, use --revision".format(rel))
        if status != 404:
            fail("cannot check {}: HTTP {} (the packages are in {})".format(rel, status, base))
        if args.dry_run:
            log("would upload {} ({} bytes, sha1 {})".format(rel, os.path.getsize(path), sha1))
            continue
        with open(path, "rb") as f:
            status, body = http("PUT", "{}/{}/{}".format(ARTIFACTORY, REPO, rel), auth, f.read(),
                                {"X-Checksum-Sha1": sha1, "Content-Type": "application/octet-stream"})
        if status != 201:
            fail("upload of {} failed: HTTP {} {}".format(rel, status, body[:200]))
        status, body = http("GET", "{}/api/storage/{}/{}".format(ARTIFACTORY, REPO, rel), auth)
        info = json.loads(body) if status == 200 else {}
        if info.get("checksums", {}).get("sha1") != sha1 or int(info.get("size", -1)) != os.path.getsize(path):
            fail("verification of {} failed".format(rel))
        log("uploaded and verified: {}/{}/{}".format(ARTIFACTORY, REPO, rel))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command")
    p = sub.add_parser("prepare", help="download, verify, prune and package the runtime of one platform")
    p.add_argument("--platform", required=True, choices=sorted(PLATFORMS))
    p.add_argument("--python", default="3.13", help="X.Y (newest patch release) or X.Y.Z (default: 3.13)")
    p.add_argument("--release", help="python-build-standalone release tag, e.g. 20261026 (default: latest)")
    p.add_argument("--revision", type=int, help="package revision, appended as -<n> (for a rebuild of a published version)")
    p.add_argument("--out", required=True, help="work directory")
    u = sub.add_parser("upload", help="publish the prepared packages on artifactory (never overwrites)")
    u.add_argument("--out", required=True, help="the work directory given to prepare")
    u.add_argument("--dry-run", action="store_true", help="only check the paths and list what would be uploaded")
    args = ap.parse_args()
    if args.command == "prepare":
        prepare(args)
    elif args.command == "upload":
        upload(args)
    else:
        ap.print_help()


if __name__ == "__main__":
    main()
