/* SPDX-License-Identifier: AGPL-3.0-only */
#include <switch.h>
#include "store.h"
#include "ca_hash.h"
#include "startup.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char source_hash[]="7c8344999b9e4f66e428eedda3b202a250ddac222d354f209385feed292754ec";
#define DEST "sdmc:/atmosphere/roomcircuit-native/CertStore/romfs.bin"
#define DIGEST "sdmc:/atmosphere/roomcircuit-native/CertStore/romfs.sha256"
static void hexhash(const void *p,size_t n,char out[65]) {
    u8 hash[32];sha256CalculateHash(hash,p,n);
    for(unsigned i=0;i<32;i++)snprintf(out+2*i,3,"%02x",hash[i]);
}
static uint8_t *readfile(const char *p,size_t *n,size_t max) {
    *n=0;FILE *f=fopen(p,"rb");if(!f)return NULL;
    uint8_t *b=NULL;
    if(fseek(f,0,SEEK_END))goto done;
    long size=ftell(f);if(size<1||(size_t)size>max||fseek(f,0,SEEK_SET))goto done;
    b=malloc((size_t)size);if(!b)goto done;
    if(fread(b,1,(size_t)size,f)!=(size_t)size||ferror(f)){free(b);b=NULL;goto done;}
    *n=(size_t)size;
done:
    fclose(f);return b;
}
static int identical_or_absent(const char *path,const void *data,size_t size) {
    struct stat st;
    if(stat(path,&st))return errno==ENOENT?0:-1;
    if(!S_ISREG(st.st_mode)||st.st_size!=(off_t)size)return -1;
    size_t n;uint8_t *b=readfile(path,&n,256*1024);
    int result=b&&n==size&&!memcmp(b,data,size)?1:-1;free(b);return result;
}
static int write_new(const char *path,const void *data,size_t size) {
    char temp[256];if(snprintf(temp,sizeof(temp),"%s.pending",path)>=(int)sizeof(temp))return -1;
    int state=identical_or_absent(path,data,size);
    if(state)return state==1?0:-1;
    FILE *f=fopen(temp,"wb");if(!f)return -1;
    int ok=fwrite(data,1,size,f)==size;
    if(fflush(f))ok=0;
    if(fsync(fileno(f)))ok=0;
    if(fclose(f))ok=0;
    if(!ok||identical_or_absent(temp,data,size)!=1)return -1;
    if(rename(temp,path))return -1;
    return identical_or_absent(path,data,size)==1?0:-1;
}
static int prepare(void) {
    if(!rcEmummcSdReady())return -1;
    Result rc=romfsInit();if(R_FAILED(rc))return -1;
    size_t cn=0,sn=0,bn=0,rn=0;uint8_t *ca=readfile("romfs:/ca.der",&cn,8192);
    uint8_t *source=NULL,*bdf=NULL,*image=NULL;char digest[66];int result=-1;
    if(!ca)goto done;
    hexhash(ca,cn,digest);if(strcmp(digest,RC_NATIVE_CA_SHA256))goto done;
    rc=romfsMountFromDataArchive(0x0100000000000800ULL,NcmStorageId_BuiltInSystem,"certstore");
    if(R_FAILED(rc)){printf("Could not read the active system data. %08lX\n",(unsigned long)rc);goto done;}
    source=readfile("certstore:/ssl_TrustedCerts.bdf",&sn,128*1024);
    romfsUnmount("certstore");
    if(!source||sn!=63532)goto done;
    hexhash(source,sn,digest);
    if(strcmp(digest,source_hash)){printf("Certificate store differs from the verified source. Nothing written.\n");goto done;}
    if(rc_store_replace_expired_root(source,sn,ca,cn,&bdf,&bn)||rc_store_romfs(bdf,bn,&image,&rn))goto done;
    hexhash(image,rn,digest);digest[64]='\n';digest[65]=0;
    if(identical_or_absent(DEST,image,rn)<0||identical_or_absent(DIGEST,digest,65)<0){
        printf("Existing private files differ. Kept unchanged.\n");goto done;
    }
    const char *dirs[]={"sdmc:/atmosphere/roomcircuit-native","sdmc:/atmosphere/roomcircuit-native/CertStore"};
    for(unsigned i=0;i<2;i++)if(mkdir(dirs[i],0777)&&errno!=EEXIST)goto done;
    if(write_new(DEST,image,rn)||write_new(DIGEST,digest,65))goto done;
    printf("Prepared %lu bytes on SD.\nSHA256: %s",(unsigned long)rn,digest);
    printf("Standard boot unchanged. No NAND writes.\n");
    result=0;
done:
    free(ca);free(source);free(bdf);free(image);romfsExit();return result;
}
int main(void) {
    if(!rcEmummcDisplayReady())return 0;
    consoleInit(NULL);
    printf("RoomCircuit Native Trust Preparation\n\n");
    printf("A: prepare private test data on the SD card\n+: exit\n");
    printf("Does not activate or change the standard boot entry.\n\n");
    padConfigureInput(1,HidNpadStyleSet_NpadStandard);PadState pad;padInitializeDefault(&pad);
    bool attempted=false;
    while(appletMainLoop()){
        padUpdate(&pad);u64 keys=padGetButtonsDown(&pad);
        if(keys&HidNpadButton_Plus)break;
        if(!attempted&&(keys&HidNpadButton_A)){
            attempted=true;if(prepare())printf("Preparation stopped. Standard boot unchanged.\n");
            printf("Press + to exit.\n");
        }
        consoleUpdate(NULL);
    }
    consoleExit(NULL);return 0;
}
