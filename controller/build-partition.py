"""Build a controller system partition from a backup of this WOW revision."""
from pathlib import Path
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent.parent
original, binary, output = map(lambda p: Path(p).resolve(), sys.argv[1:4])
e2 = Path("/opt/homebrew/opt/e2fsprogs/sbin")
debugfs = shutil.which("debugfs") or str(e2 / "debugfs")
fsck = shutil.which("e2fsck") or str(e2 / "e2fsck")
original_partition_hash = "68163fad670ad3b2c4ebbd6f1a5e61e72d3afc5a33648f6fad31ce3d4f0e29c5"
stock = b"/bin/gadget_msg.sh startup &\n/bin/serve wowmachine &"
branch = b"""if [ "$Sample" = "1" ] && [ "$Beat" != "1" ] && [ "$Motor" != "1" ]; then
    /bin/gadget_msg.sh startup &
    /bin/serve wowmachine &
else
    (
        log=/dev/console
        if grep -qs ' /var/nona vfat ' /proc/mounts; then log=/var/nona/wow-midi.log; fi
        mode=normal
        if [ "$Beat" = "1" ] && [ "$Sample" != "1" ] && [ "$Motor" != "1" ]; then mode=rescue; fi
        /bin/wow-boot "$mode" >"$log" 2>&1 && exit 0
        sync
        /bin/gadget_msg.sh startup &
        exec /bin/serve wowmachine
    ) &
fi"""
if output.exists():
    raise SystemExit(f"Output already exists: {output}")
def check_fs(image, repair):
    # This exact stock hash has an unlinked zero-byte inode (466) on the orphan list.
    # Replay the journal and clear it on the private image copy.
    r = subprocess.run([fsck, "-f", "-y" if repair else "-n", str(image)], check=False)
    if r.returncode not in ((0, 1) if repair else (0,)):
        raise RuntimeError(f"Filesystem check exited {r.returncode}")
with tempfile.TemporaryDirectory() as temp:
    work = Path(temp)
    image = work / "system.img"
    with original.open("rb") as source:
        mbr = source.read(512)
        sector, count = struct.unpack_from("<II", mbr, 446 + 8)
        if mbr[510:] != b"\x55\xaa" or (sector, count) != (2048, 262144):
            raise ValueError("Unrecognized card partition map")
        source.seek(sector * 512)
        data = source.read(count * 512)
    if hashlib.sha256(data).hexdigest() != original_partition_hash:
        raise ValueError("Unrecognized WOW system partition")
    image.write_bytes(data)
    del data
    def extract(guest, local):
        subprocess.run([debugfs, "-R", f"dump {guest} {local.name}", str(image)],
                       cwd=work, stdout=subprocess.DEVNULL, check=True)
        return local.read_bytes()
    init = extract("/bin/startup.sh", work/"stock.sh")
    if init.count(stock) != 1:
        raise ValueError("Unrecognized startup sequence")
    filenames = ["startup.sh", "wow-midi-mode", "wow-midi", "wow-library", "wow-usb", "wow-update", "wow-boot"]
    commands = ["rm /bin/startup.sh"]
    # An optional current system image preserves any earlier on-device changes.
    if len(sys.argv) == 5:
        current = Path(sys.argv[4]).resolve()
        if current.stat().st_size != count * 512:
            raise ValueError("Unexpected size for the current system")
        shutil.copyfile(current, image)
    check_fs(image, repair=True)
    if len(sys.argv) == 5:
        observed = extract("/bin/startup.sh", work/"current.sh")
        if observed == init.replace(stock, branch):
            commands.extend("rm /bin/"+name for name in filenames if name != "startup.sh")
        elif observed != init:
            if hashlib.sha256(observed).hexdigest() not in ("3affe5f6895cb7264eae768ee33540a9e91809d529f2ef8dd3d86e4ac86d819e", "3e5372c5b0ad5b38f737a15a9a241f34cc831fb38dda13d97360707b73621358"):
                raise ValueError("Current startup script differs from both supported revisions")
            commands.extend(["rm /bin/wow-midi-mode", "rm /bin/wow-midi"])
    # Assert all new paths are absent in the known image. Set Unix metadata
    # explicitly because the host development drive is formatted as exFAT.
    (work/"startup.sh").write_bytes(init.replace(stock, branch))
    shutil.copyfile(binary, work/"wow-midi")
    shutil.copyfile(root/"controller/wow-midi-mode", work/"wow-midi-mode")
    for name in ["wow-library", "wow-update", "wow-usb"]:
        shutil.copyfile(root/"build"/name, work/name)
    shutil.copyfile(root/"controller/wow-boot", work/"wow-boot")
    for name in ["wow-midi", "wow-library", "wow-usb", "wow-update"]:
        data=(work/name).read_bytes()
        if data[:7]!=b"\x7fELF\x01\x01\x01" or data[18:20]!=b"\x28\x00":
            raise ValueError(f"{name} is not an ARM executable")
    for name in filenames:
        commands += [f"write {name} /bin/{name}",
                     f"set_inode_field /bin/{name} mode 0100755",
                     f"set_inode_field /bin/{name} uid 0",
                     f"set_inode_field /bin/{name} gid 0"]
    subprocess.run([debugfs, "-w", "-f", "-", str(image)], input="\n".join(commands)+"\n",
                   text=True, cwd=work, check=True, stdout=subprocess.DEVNULL)
    check_fs(image, repair=False)
    for name in filenames:
        if extract("/bin/"+name, work/(name+".check")) != (work/name).read_bytes():
            raise RuntimeError(f"Verification failed for {name}")
    subprocess.run(["sh", "-n", str(work/"startup.sh")], check=True)
    subprocess.run(["sh", "-n", str(work/"wow-midi-mode")], check=True)
    with image.open("rb") as src, output.open("xb") as dst:
        shutil.copyfileobj(src, dst)
        dst.flush()
        os.fsync(dst.fileno())
    with image.open("rb") as src, output.open("rb") as dst:
        expected = hashlib.file_digest(src, "sha256").hexdigest()
        if hashlib.file_digest(dst, "sha256").hexdigest() != expected:
            raise IOError("Partition copy verification failed")
    print(output, expected)
