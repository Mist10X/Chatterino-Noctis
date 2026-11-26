"""Sign an independent Noctis Moderator extension package. Never publishes.

Uses the existing protected owner signing key, not a separate access grant.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path

spec = importlib.util.spec_from_file_location("sign_module", Path(__file__).with_name("sign-module.py"))
signer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(signer)

parser = argparse.ArgumentParser()
parser.add_argument("--spec", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--key-dir", type=Path, default=Path(os.environ["LOCALAPPDATA"])/"NoctisModeratorSigning")
args = parser.parse_args()
payload = json.loads(args.spec.read_text(encoding="utf-8"))
assert payload["type"] == "noctis-moderator-extension" and payload["schema"] == 1 and payload["host_api"] in (1,2)
required = {"type","schema","host_api","revision","version","labels","pages","reward_actions"}
assert required <= set(payload) <= required | ({"layout"} if payload["host_api"] == 2 else set())
raw = json.dumps(payload, ensure_ascii=False, separators=(",", ":"), sort_keys=True).encode("utf-8")
assert len(raw) <= 80*1024
pem = signer.dpapi((args.key_dir/"ed25519.dpapi").read_bytes(), False)
assert signer.b64(signer.public_key(pem)) == "3GB9OcyrH3zdcuR5xZAkeoyhQgO1AhhHpsJmWnU8owM="
signed = {"payload": signer.b64(raw), "signature": signer.b64(signer.sign(pem, raw, args.key_dir))}
del pem
args.output.parent.mkdir(parents=True, exist_ok=True)
with args.output.open("x", encoding="utf-8") as f:
    json.dump(signed, f, ensure_ascii=False, indent=2)
    f.write("\n")
print("Signed extension", payload["version"], "revision", payload["revision"])
