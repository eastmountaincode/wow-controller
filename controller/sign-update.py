"""Sign a controller release. The signing key stays on the development Mac."""
import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path

components = ("wow-midi-mode", "wow-midi", "wow-library", "wow-usb")
root = Path(__file__).resolve().parents[1]
keydir = Path.home() / "Library/Application Support/WOW Controller"
private = keydir / "controller-ed25519.pem"
public = root / "controller/owner-public.key"
openssl = "/opt/homebrew/bin/openssl"

def run(*args, data=None):
    return subprocess.check_output([openssl, *map(str,args)], input=data)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--init-key", action="store_true")
    parser.add_argument("output", type=Path, nargs="?")
    args = parser.parse_args()
    os.umask(0o077)
    if args.init_key:
        keydir.mkdir(mode=0o700,parents=True,exist_ok=True)
        if not private.exists():
            if public.exists():
                raise SystemExit("Restore the matching private key before signing.")
            with private.open("xb") as f:
                f.write(run("genpkey","-algorithm","ED25519"))
                f.flush(); os.fsync(f.fileno())
        der=run("pkey","-in",private,"-pubout","-outform","DER")
        if len(der)!=44 or der[:12]!=bytes.fromhex("302a300506032b6570032100"):
            raise ValueError("Unexpected Ed25519 public key encoding")
        if not public.exists():
            with public.open("xb") as f: f.write(der[-32:])
        if public.read_bytes()!=der[-32:]: raise ValueError("Signing key mismatch")
    if not args.output: return
    if run("pkey","-in",private,"-pubout","-outform","DER")[-32:]!=public.read_bytes():
        raise ValueError("Signing key mismatch")
    blobs=[((root/"controller" if name=="wow-midi-mode" else root/"build")/name).read_bytes() for name in components]
    body=b"WOWCTRL1"+struct.pack("<4I",*[len(b) for b in blobs])+b"".join(blobs)
    if min(map(len,blobs))==0 or len(body)+64>8*1024*1024: raise ValueError("Invalid release size")
    with tempfile.NamedTemporaryFile() as tmp:
        tmp.write(body);tmp.flush()
        sig=run("pkeyutl","-sign","-rawin","-inkey",private,"-in",tmp.name)
    if len(sig)!=64: raise ValueError("Invalid Ed25519 signature size")
    with args.output.open("xb") as f:
        f.write(body+sig);f.flush();os.fsync(f.fileno())

if __name__=="__main__": main()
