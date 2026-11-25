"""Create/sign the Noctis Moderator manifest. Never pushes or publishes.

The signing key is stored encrypted with Windows per-user DPAPI, outside the repo.
Requires Python 3 and an OpenSSL 3 executable on PATH.
"""
import argparse
import base64
import ctypes
from ctypes import wintypes
import datetime as dt
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


class Blob(ctypes.Structure):
    _fields_ = [("size", wintypes.DWORD), ("data", ctypes.POINTER(ctypes.c_ubyte))]


def dpapi(data, protect):
    buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
    source = Blob(len(data), buf)
    out = Blob()
    crypt = ctypes.windll.crypt32
    crypt.CryptProtectData.argtypes = [ctypes.POINTER(Blob), wintypes.LPCWSTR,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(Blob)]
    crypt.CryptUnprotectData.argtypes = [ctypes.POINTER(Blob), ctypes.c_void_p,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(Blob)]
    ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    ctypes.windll.kernel32.LocalFree.restype = ctypes.c_void_p
    if protect:
        ok = crypt.CryptProtectData(ctypes.byref(source), "Noctis Moderator signing key",
                                     None, None, None, 1, ctypes.byref(out))
    else:
        ok = crypt.CryptUnprotectData(ctypes.byref(source), None,
                                       None, None, None, 1, ctypes.byref(out))
    if not ok:
        raise ctypes.WinError()
    try:
        return ctypes.string_at(out.data, out.size)
    finally:
        ctypes.windll.kernel32.LocalFree(out.data)


def run(*args, data=None):
    return subprocess.run(["openssl", *args], input=data,
                          capture_output=True, check=True).stdout


def b64(data):
    return base64.b64encode(data).decode("ascii")


def public_key(pem):
    der = run("pkey", "-pubout", "-outform", "DER", data=pem)
    if len(der) != 44 or der[:12] != bytes.fromhex("302a300506032b6570032100"):
        raise ValueError("not an Ed25519 key")
    return der[-32:]


def sign(pem, payload, private_dir):
    # OpenSSL Ed25519 needs seekable input. The transient key stays under the
    # user's private AppData directory and is removed on normal exit.
    with tempfile.TemporaryDirectory(prefix="sign-", dir=private_dir) as path:
        folder = Path(path)
        key = folder / "key.pem"
        message = folder / "payload.bin"
        key.write_bytes(pem)
        message.write_bytes(payload)
        return run("pkeyutl", "-sign", "-rawin", "-inkey", str(key),
                   "-in", str(message))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--init-key", action="store_true")
    parser.add_argument("--key-dir", type=Path, default=Path(os.environ["LOCALAPPDATA"])/"NoctisModeratorSigning")
    parser.add_argument("--spec", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    args.key_dir.mkdir(parents=True, exist_ok=True)
    key_file = args.key_dir / "ed25519.dpapi"
    if args.init_key:
        if key_file.exists():
            raise SystemExit("Signing key already exists; refusing to replace it.")
        pem = run("genpkey", "-algorithm", "ED25519")
        with key_file.open("xb") as f:
            f.write(dpapi(pem, True))
        print("Public key (base64):", b64(public_key(pem)))
        return
    if not args.spec or not args.output:
        parser.error("--spec and --output required")
    pem = dpapi(key_file.read_bytes(), False)
    spec = json.loads(args.spec.read_text(encoding="utf-8"))
    ids = spec["allowed_twitch_ids"]
    if len(ids) > 500 or not all(isinstance(i, str) and re.fullmatch(r"[1-9][0-9]{0,19}", i) for i in ids):
        raise ValueError("allowed_twitch_ids must contain Twitch numeric IDs")
    source_file = args.spec.parent / spec["engine"]
    source = source_file.read_text(encoding="utf-8")
    if len(source.encode("utf-8")) > 16384:
        raise ValueError("engine too large")
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0)
    payload = dict(schema=1, revision=spec["revision"], module_version=spec["module_version"],
                   issued_at=now.isoformat(), expires_at=(now+dt.timedelta(days=90)).isoformat(),
                   allowed_twitch_ids=sorted(set(ids)), engine=source,
                   min_cooldown_seconds=spec["min_cooldown_seconds"],
                   max_rules=spec["max_rules"])
    raw = json.dumps(payload, ensure_ascii=False, separators=(",", ":"), sort_keys=True).encode("utf-8")
    signed = dict(payload=b64(raw), signature=b64(sign(pem, raw, args.key_dir)))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(signed, indent=2)+"\n", encoding="utf-8")
    print("Signed revision", spec["revision"], "for", len(ids), "Twitch IDs.")


if __name__ == "__main__":
    main()
