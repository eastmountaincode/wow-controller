#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "wow-keyboard-payload.h"
#include <unistd.h>

/* FunctionFS remaps the interface and endpoint numbers inside the composite
 * configuration. The WOW is little-endian, as are USB descriptor fields. */
struct speed_descriptors {
    struct usb_interface_descriptor interface;
    struct usb_endpoint_descriptor_no_audio out, in;
} __attribute__((packed));

static struct speed_descriptors descriptors(unsigned packet) {
    struct speed_descriptors d = {
        .interface = { .bLength=USB_DT_INTERFACE_SIZE, .bDescriptorType=USB_DT_INTERFACE,
            .bNumEndpoints=2, .bInterfaceClass=USB_CLASS_VENDOR_SPEC,
            .bInterfaceSubClass=0x57, .bInterfaceProtocol=1, .iInterface=1 },
        .out = { .bLength=USB_DT_ENDPOINT_SIZE, .bDescriptorType=USB_DT_ENDPOINT,
            .bEndpointAddress=1, .bmAttributes=USB_ENDPOINT_XFER_BULK, .wMaxPacketSize=packet },
        .in = { .bLength=USB_DT_ENDPOINT_SIZE, .bDescriptorType=USB_DT_ENDPOINT,
            .bEndpointAddress=USB_DIR_IN|2, .bmAttributes=USB_ENDPOINT_XFER_BULK, .wMaxPacketSize=packet }
    };
    return d;
}

static int register_function(int ep) {
    struct {
        struct usb_functionfs_descs_head_v2 header;
        uint32_t fs_count, hs_count;
        struct speed_descriptors fs, hs;
    } __attribute__((packed)) d = {
        .header={FUNCTIONFS_DESCRIPTORS_MAGIC_V2,sizeof(d),FUNCTIONFS_HAS_FS_DESC|FUNCTIONFS_HAS_HS_DESC},
        .fs_count=3,.hs_count=3,.fs=descriptors(64),.hs=descriptors(512)
    };
    struct {
        struct usb_functionfs_strings_head header;
        uint16_t lang;
        char name[sizeof("WOW Library")];
    } __attribute__((packed)) s = {{FUNCTIONFS_STRINGS_MAGIC,sizeof(s),1,1},0x0409,"WOW Library"};
    return write(ep,&d,sizeof(d))==sizeof(d) && write(ep,&s,sizeof(s))==sizeof(s);
}

static volatile sig_atomic_t stop;
static void stop_signal(int sig) { (void)sig; stop=1; }

static void end_session(pid_t *pid) {
    if (*pid <= 0) return;
    if (waitpid(*pid,NULL,WNOHANG)==0) {
        kill(*pid,SIGTERM);
        for (int i=0; i<20; ++i) {
            if (waitpid(*pid,NULL,WNOHANG)!=0) { *pid=0; return; }
            usleep(5000);
        }
        kill(*pid,SIGKILL);
        waitpid(*pid,NULL,0);
    }
    *pid=0;
}

static pid_t start_session(const char *program) {
    int rx=open("ep1",O_RDONLY|O_CLOEXEC), tx=open("ep2",O_WRONLY|O_CLOEXEC);
    pid_t pid=-1;
    if (rx>=0 && tx>=0) {
        ioctl(rx,FUNCTIONFS_FIFO_FLUSH);
        ioctl(tx,FUNCTIONFS_FIFO_FLUSH);
        pid=fork();
        if (pid==0) {
            if (dup2(rx,STDIN_FILENO)<0 || dup2(tx,STDOUT_FILENO)<0) _exit(126);
            execl(program,program,"--stdio",(char *)NULL);
            _exit(127);
        }
    }
    if (rx>=0) close(rx);
    if (tx>=0) close(tx);
    return pid;
}

/* The card's existing updater verifies exactly four signed components. Bundle
 * the adapter in this one, and materialize it before the engine starts. */
static int prepare_keyboard(const char *directory) {
    if (chdir(directory) < 0) return 1;
    const char *name = "wow-keyboard.so", *next = ".wow-keyboard.so.next";
    struct stat st;
    int fd = open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    size_t at = 0;
    if (fd >= 0) {
        if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == sizeof wow_keyboard_payload) {
            unsigned char buf[4096];
            while (at < sizeof wow_keyboard_payload) {
                ssize_t n = read(fd, buf, sizeof buf);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0 || memcmp(buf, wow_keyboard_payload + at, (size_t)n)) break;
                at += (size_t)n;
            }
        }
        close(fd);
        if (at == sizeof wow_keyboard_payload) return 0;
    }
    if (unlink(next) && errno != ENOENT) return 1;
    fd = open(next, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return 1;
    for (at = 0; at < sizeof wow_keyboard_payload;) {
        ssize_t n = write(fd, wow_keyboard_payload + at, sizeof wow_keyboard_payload - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        at += (size_t)n;
    }
    int failed = at != sizeof wow_keyboard_payload || fsync(fd);
    if (close(fd) || failed || rename(next, name)) { unlink(next); return 1; }
    int dir = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return 1;
    failed = fsync(dir);
    close(dir);
    return failed != 0;
}

int main(int argc,char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--prepare-keyboard")) return prepare_keyboard(argv[2]);
    if(argc!=2 || chdir("/dev/wow-library")) return 1;
    int ep=open("ep0",O_RDWR|O_CLOEXEC);
    if(ep<0 || !register_function(ep)) { perror("register WOW Library USB"); return 1; }
    struct sigaction sa={.sa_handler=stop_signal};
    sigaction(SIGTERM,&sa,NULL); sigaction(SIGINT,&sa,NULL);
    pid_t child=0;
    while(!stop) {
        struct usb_functionfs_event ev;
        ssize_t n=read(ep,&ev,sizeof(ev));
        if(n<0 && (errno==EINTR || errno==EIDRM)) continue;
        if(n!=sizeof(ev)) break;
        if(ev.type==FUNCTIONFS_DISABLE || ev.type==FUNCTIONFS_UNBIND) end_session(&child);
        if(ev.type!=FUNCTIONFS_SETUP) continue;
        const struct usb_ctrlrequest *req=&ev.u.setup;
        char unused=0;
        if(req->bRequestType==(USB_TYPE_VENDOR|USB_RECIP_INTERFACE|USB_DIR_OUT) &&
           req->bRequest==0x51 && req->wLength==0 && req->wValue==0) {
            end_session(&child);
            child=start_session(argv[1]);
            if(child>0) { (void)read(ep,&unused,0); continue; }
        }
        /* An IO in the opposite direction stalls unsupported control requests. */
        if(req->bRequestType & USB_DIR_IN) (void)read(ep,&unused,0);
        else (void)write(ep,&unused,0);
    }
    end_session(&child);
    close(ep);
    return stop ? 0 : 1;
}
