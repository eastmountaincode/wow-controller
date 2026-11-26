"""Build the WOW's ARM controller and sign a release for USB installation."""
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
cached_zig = Path.home() / "Library/Caches/WOW Controller/zig-aarch64-macos-0.14.1/zig"
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--zig", default=os.environ.get("WOW_ZIG") or (str(cached_zig) if cached_zig.is_file() else shutil.which("zig")), help="Path to Zig 0.14.1")
parser.add_argument("output",type=Path,help="New .upd file")
args=parser.parse_args()
if not args.zig or subprocess.check_output([args.zig,"version"],text=True).strip()!="0.14.1":
    parser.error("Supply --zig pointing to Zig 0.14.1")
output=args.output.resolve()
if output.exists(): parser.error("Choose a new output filename")
def run(*argv): subprocess.run(argv,cwd=root,check=True)
target=["-target","arm-linux-musleabihf"]
build=root/"build"
build.mkdir(exist_ok=True)
cc=[args.zig,"cc",*target,"-mcpu=cortex_a7","-std=c11","-Wall","-Wextra","-Werror","-Os","-static","-s"]
epoch = os.environ.get("SOURCE_DATE_EPOCH")
built_at = datetime.fromtimestamp(int(epoch), timezone.utc) if epoch else datetime.now(timezone.utc)
stamp = built_at.strftime("%Y-%m-%dT%H:%M:%SZ")
with tempfile.TemporaryDirectory() as temp:
    artifacts=Path(temp)
    adapter=artifacts/"wow-keyboard.so"
    run(args.zig,"cc","-target","arm-linux-gnueabihf.2.31","-mcpu=cortex_a7",
        "-std=c11","-Wall","-Wextra","-Werror","-Os","-shared","-fPIC","-s",
        "controller/wow-keyboard.c","-pthread","-ldl","-Wl,-z,defs","-Wl,-soname,wow-keyboard.so","-o",adapter)
    payload=adapter.read_bytes()
    if payload[:7]!=b"\x7fELF\x01\x01\x01" or payload[16:20]!=b"\x03\x00\x28\x00":
        raise ValueError("Invalid ARM keyboard adapter")
    rows=(", ".join(f"0x{v:02x}" for v in payload[i:i+24]) for i in range(0,len(payload),24))
    (artifacts/"wow-keyboard-payload.h").write_text(
        "static const unsigned char wow_keyboard_payload[] = {\n" + ",\n".join(rows) + "\n};\n")
    verify=str(artifacts/"verify.o")
    run(args.zig,"build-obj","controller/wow-verify.zig",*target,"-mcpu","cortex_a7","-O","ReleaseSmall","-femit-bin="+verify)
    run(*cc,"controller/wow-update.c",verify,"-o",artifacts/"wow-update")
    for name in ["wow-midi","wow-library","wow-usb"]:
        defines = [f'-DWOW_BUILD_UTC="{stamp}"'] if name == "wow-library" else []
        run(*cc,"-I",artifacts,*defines,root/"controller"/(name+".c"),"-lm","-o",artifacts/name)
    # Link on the Mac's native filesystem. Verify a flushed byte copy on the
    # removable project volume, including the ELF machine and executable type.
    for name in ["wow-update", "wow-midi", "wow-library", "wow-usb"]:
        data=(artifacts/name).read_bytes()
        if data[:7]!=b"\x7fELF\x01\x01\x01" or data[18:20]!=b"\x28\x00":
            raise ValueError(f"{name} is not a little-endian 32-bit ARM ELF")
        pending=build/(name+".copy")
        with pending.open("wb") as f:
            f.write(data);f.flush();os.fsync(f.fileno())
        os.replace(pending,build/name)
        if (build/name).read_bytes()!=data: raise IOError(f"Copy failed for {name}")
run(sys.executable,root/"controller/sign-update.py",output)
print(output)
