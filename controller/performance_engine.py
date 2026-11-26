"""Build the performance controls for one verified WOW audio engine, at build time."""
import gzip
import hashlib
from pathlib import Path
import shutil
import subprocess


STOCK_SHA256 = "26e08d4431ce563a530decd40bf56f001efc3a92a23407c64f75222db67873af"
PERFORMANCE_SHA256 = "2c5d3c24b438ecf0b7c4646fedc51a21c9df6a0787378c233fa9f17938b495f9"


def factory_audio_engine(executable, system_image, work):
    if executable:
        return executable.read_bytes()
    if not system_image.is_file():
        raise ValueError("Supply the extracted factory /bin/wowmachine with --factory-engine")
    debugfs = shutil.which("debugfs") or "/opt/homebrew/opt/e2fsprogs/sbin/debugfs"
    with gzip.open(system_image, "rb") as source, (work / "system.img").open("wb") as image:
        shutil.copyfileobj(source, image)
    subprocess.run([debugfs, "-R", "dump /bin/wowmachine engine", "system.img"], cwd=work, check=True)
    return (work / "engine").read_bytes()


def performance_engine(stock):
    if hashlib.sha256(stock).hexdigest() != STOCK_SHA256:
        raise ValueError("Unrecognized factory audio engine")
    engine = bytearray(stock)
    # WowDeck::onKeyPressed, ARM Thumb. The original START modifier checks
    # and motor toggle stay intact. Dispatch BEAT and the panel's private FX
    # key in the old START-double-tap interval calculation's space.
    engine[0xdd52:0xdd56] = bytes.fromhex("00f0 f9b8")  # b.w key_dispatch
    engine[0xdf46:0xdf5e] = bytes.fromhex(
        "15e0 "       # after the stock motor toggle, b 0xdf74 (return)
        "a52c "       # key_dispatch: cmp r4, #165 (BEAT)
        "3ff4 04af "  # beq.w 0xdd56 (original BEAT)
        "b4f5 007f "  # cmp.w r4, #512 (panel FX)
        "7ff4 10af "  # bne.w 0xdd76 (original unknown key)
        "05f5 4054 "  # add.w r4, r5, #0x3000 (factory filter fields)
        "00f0 8db8"   # b.w 0xe078 (original FX toggle)
    )
    # FX completes via the normal return, independently of the motor code.
    engine[0xe0ac:0xe0ae] = bytes.fromhex("62e7")  # b 0xdf74
    # A plain BEAT release selects the next file. Both native C++ release
    # handlers already check the BEAT modifier before reaching these branches.
    # Use their original loadBeat(index+1) and writeLastTrackId paths while
    # preserving the play flag, including when the backing beat is stopped.
    engine[0xfa00:0xfa02] = bytes.fromhex("02e0")  # b 0xfa08 (deck handler)
    engine[0xfac2:0xfac4] = bytes.fromhex("01e0")  # b 0xfac8 (keyboard thunk)
    engine[0x180:0x194] = hashlib.sha1(engine).digest()  # distinct GNU build ID
    if hashlib.sha256(engine).hexdigest() != PERFORMANCE_SHA256:
        raise ValueError("Unexpected performance audio engine fingerprint")
    return bytes(engine)
