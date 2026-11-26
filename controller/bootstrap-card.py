"""Install the first USB library build onto this WOW's known microSD via the TS4."""
from pathlib import Path
import array
import errno
import fcntl
import hashlib
import os
import plistlib
import socket
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
backup=root/"backup/WOW-original-2026-11-25.img"
diskutil="/usr/sbin/diskutil"
size=134217728
def info(identifier):
    return plistlib.loads(subprocess.check_output([diskutil,"info","-plist",identifier]))
def identify():
    disks=plistlib.loads(subprocess.check_output([diskutil,"list","-plist","external","physical"]))
    candidates=[]
    for name in disks["WholeDisks"]:
        whole=info(name)
        if whole.get("MediaName")!="TS4 Card Reader" or whole["Size"]!=8063549440 or whole["Internal"]: continue
        for i, (kind,offset,n) in enumerate([("Linux",1048576,size),("DOS_FAT_32",135266304,6442450944),("Linux_Swap",6577717248,1073741824)],1):
            part=info(f"{name}s{i}")
            if (part["Content"],part["PartitionMapPartitionOffset"],part["Size"])!=(kind,offset,n) or part["ParentWholeDisk"]!=name:
                raise RuntimeError("Unexpected card layout")
        candidates.append(name)
    if len(candidates)!=1: raise RuntimeError("Insert the WOW microSD into the TS4 card reader")
    return candidates[0]
def open_raw(node):
    a,b=socket.socketpair()
    pr=subprocess.Popen(["/usr/libexec/authopen","-stdoutpipe","-o","2",node],stdout=b)
    b.close()
    with a: _,controls,_,_=a.recvmsg(1024,socket.CMSG_SPACE(4))
    fds=array.array("i")
    for level,kind,data in controls:
        if level==socket.SOL_SOCKET and kind==socket.SCM_RIGHTS: fds.frombytes(data[:4])
    if pr.wait()!=0 or len(fds)!=1: raise PermissionError("macOS raw card access was not granted")
    return os.fdopen(fds[0],"r+b",buffering=0)
def readcard(card):
    card.seek(0)
    out=bytearray()
    while len(out)<size:
        data=card.read(min(1048576,size-len(out)))
        if not data: raise IOError("Short read from system partition")
        out.extend(data)
    return out
def writecard(card,data):
    card.seek(0);view=memoryview(data)
    while len(view):
        n=card.write(view[:1048576])
        if not n: raise IOError("Short write to system partition")
        view=view[n:]
    flushed=False
    for action in (lambda:os.fsync(card.fileno()),lambda:fcntl.ioctl(card.fileno(),0x20006416)):
        try: action();flushed=True
        except OSError as exc:
            if exc.errno not in (errno.ENOTTY,errno.ENOTSUP,errno.EINVAL): raise
    if not flushed or readcard(card)!=data: raise IOError("Card verification failed")
def main():
    name=identify()
    with backup.open("rb") as f:
        if hashlib.file_digest(f,"sha256").hexdigest()!="79e23d40e262802451a3cc4c306a3c609583808969f1d2eeb74edb47c09d6e16":
            raise ValueError("Factory backup verification failed")
    subprocess.run([diskutil,"unmountDisk",name],check=True)
    node=f"/dev/r{name}s1"
    with open_raw(node) as card,tempfile.TemporaryDirectory() as tmp:
        if identify()!=name or os.fstat(card.fileno()).st_rdev!=os.stat(node).st_rdev: raise RuntimeError("Card changed")
        old=readcard(card)
        previous=Path(tmp)/"current.img";previous.write_bytes(old)
        image=Path(tmp)/"candidate.img"
        subprocess.run([sys.executable,root/"controller/build-partition.py",backup,root/"build/wow-midi",image,previous],check=True)
        debugfs="/opt/homebrew/opt/e2fsprogs/sbin/debugfs"
        for component in ("wow-midi","wow-library","wow-usb","wow-update"):
            for location in (previous,image):
                payload=subprocess.check_output([debugfs,"-R","cat /bin/"+component,location],stderr=subprocess.DEVNULL)
                if location==previous:
                    print("Previous",component,len(payload),"ARM ELF" if payload[:4]==b"\x7fELF" else "unavailable",flush=True)
                elif payload[:7]!=b"\x7fELF\x01\x01\x01" or payload[18:20]!=b"\x28\x00" or payload!=(root/"build"/component).read_bytes():
                    raise ValueError("Invalid payload: "+component)
        new=image.read_bytes()
        if len(new)!=size or readcard(card)!=old: raise ValueError("Card or image changed")
        try: writecard(card,new)
        except BaseException:
            print("Restoring the previous system partition.",flush=True)
            writecard(card,old);raise
        saved=root/"build/WOW-library-system.tmp"
        with saved.open("wb") as dest:
            dest.write(new);dest.flush();os.fsync(dest.fileno())
        if saved.read_bytes()!=new: raise IOError("Saved image verification failed")
        os.replace(saved, root/"build/WOW-library-system.img")
        print("Verified installed partition",hashlib.sha256(new).hexdigest(),flush=True)
    subprocess.run([diskutil,"eject",name],check=True)

if __name__=="__main__": main()
