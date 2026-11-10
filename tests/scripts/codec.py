# Functions called by tst_nrpycpp.cpp
import helper  # from the dep/ folder, passed to PyRunnerQt as dependency


def echo(*args):
    return list(args)


def types(*args):
    return [type(a).__name__ for a in args]


def edge_values():
    return [2**40, -2**40, 2**64 - 1, 0.1 + 0.2, "quote ' and \" \n and ünïcödé",
            (1, 2), {"a": 1, "b": [b"\x00\x01"]}, None, True]


def too_big():
    return 2**64


def boom(x):
    return helper.divide(x, 0)


def crc(data):
    import zlib
    return zlib.crc32(data)


def unsupported_return():
    return object()


def stdlib_check():
    # modules a packet / codec prototype is likely to need: they must be in the bundled runtime
    import binascii, ctypes, hashlib, json, re, socket, ssl, struct, zlib, sqlite3, decimal
    return [hashlib.sha256(b"abc").hexdigest(), struct.pack("<HI", 1, 2), ssl.OPENSSL_VERSION.split()[0]]


def venv_check():
    import tinypkg
    return tinypkg.VALUE


def say(text):
    print(text)
    return len(text)


def python_version():
    import sys
    return "{}.{}.{}".format(*sys.version_info[:3])
