#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int wow_verify(const unsigned char *,size_t,const unsigned char *);
static const char *components[]={"wow-midi-mode","wow-midi","wow-library","wow-usb"};

static int save_file(int dir,const char *name,const unsigned char *data,size_t len,mode_t mode) {
    int f=openat(dir,name,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,mode);
    if(f<0) return -1;
    while(len) {
        ssize_t n=write(f,data,len);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { close(f); return -1; }
        len-=(size_t)n; data+=n;
    }
    int err=fsync(f);
    if(close(f)<0) return -1;
    return err;
}

static int install(int root,const unsigned char *buf,size_t size) {
    char current[32]={0};
    ssize_t n=readlinkat(root,"active",current,sizeof(current)-1);
    if(n<0 && errno!=ENOENT) return -1;
    if(n>=0 && strcmp(current,"slot-a") && strcmp(current,"slot-b")) return -1;
    const char *slot=!strcmp(current,"slot-a")?"slot-b":"slot-a";
    if(mkdirat(root,slot,0755)<0 && errno!=EEXIST) return -1;
    int dir=openat(root,slot,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(dir<0) return -1;
    size_t pos=24;
    int ok=-1;
    for(size_t i=0;i<4;++i) {
        uint32_t len=0;
        for(int b=0;b<4;b++) len|=(uint32_t)buf[8+i*4+b]<<(b*8);
        if(!len || len>size-64-pos || save_file(dir,components[i],buf+pos,len,0755)) goto done;
        pos+=len;
    }
    if(pos!=size-64) goto done;
    char id[14];
    for(int i=0;i<6;i++) snprintf(id+2*i,3,"%02x",buf[pos+i]);
    id[12]='\n';
    if(save_file(dir,"release-id",(unsigned char *)id,13,0644) || fsync(dir)) goto done;
    if(unlinkat(root,"active.next",0)<0 && errno!=ENOENT) goto done;
    if(symlinkat(slot,root,"active.next") || renameat(root,"active.next",root,"active") || fsync(root)) goto done;
    ok=0;
done:
    close(dir);
    return ok;
}

int main(int argc,char **argv) {
    if(argc!=3) return 2;
    int fd=open(argv[1],O_RDONLY|O_NOFOLLOW);
    struct stat st;
    if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<88 || st.st_size>8*1024*1024) return 2;
    size_t size=(size_t)st.st_size;
    unsigned char *buf=malloc(size);
    if(!buf) return 1;
    size_t pos=0;
    while(pos<size) {
        ssize_t n=read(fd,buf+pos,size-pos);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) return 1;
        pos+=(size_t)n;
    }
    close(fd);
    if(memcmp(buf,"WOWCTRL1",8) || !wow_verify(buf,size-64,buf+size-64)) {
        fprintf(stderr,"Update has an invalid format or signature.\n");return 2;
    }
    int root=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(root<0 || install(root,buf,size)) { perror("Install controller release");return 1; }
    close(root);free(buf);return 0;
}
