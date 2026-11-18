// SPDX-License-Identifier: GPL-2.0-only
// Exercise the pinned NX Pocket discovery and session code with kernel TCP and UDP.
// The test supplies only local-interface selection, HLE registration and OS event hooks.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint32_t local;
typedef uint32_t Result;
#define RES_OK 0
#define RES_FAIL 0x2a2
#define HLE_FORCE(symbol, reason, type, name, ...) static type name(__VA_ARGS__)
#define LOG(...) do { if (getenv("RC_TRACE")) {fprintf(stderr, __VA_ARGS__);fputc('\n',stderr);} } while (0)
static int dm_net_on(void) {return 1;}
static int dm_net_unicast(void) {return 1;}
static int dm_net_log(void) {return getenv("RC_TRACE") != NULL;}
static int dm_net_local(uint32_t *ip, uint32_t *mask) {*ip = local; *mask = 0xfffffffc; return 1;}
static int dm_net_peers(uint32_t *ips, uint16_t *ports, int max) {
    if (!max) return 0;
    ips[0] = 0x0a0dfefe;
    if (ports) ports[0] = 11452;
    return 1;
}
static void dm_net_set_room(const uint32_t *ip, int count) {(void)ip; (void)count;}
static void hle_signal_system_event(void *event) {(void)event;}
static void hle_rebind_system_event(void *event, int active) {(void)event; (void)active;}
static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000ull + t.tv_nsec;
}
void dm_guest_blocking(int value) {(void)value;}
void dm_guest_sleep_ns(int64_t ns) {
    struct timespec t = {ns / 1000000000, ns % 1000000000}; nanosleep(&t, NULL);
}

#include "test-hle-ldn.inc"

static void ensure(int condition, const char *label) {
    if (!condition) { fprintf(stderr, "NX reference failed at %s\n", label); exit(1); }
}

int main(int argc, char **argv) {
    ensure(argc == 3, "arguments");
    int host = !strcmp(argv[1], "host");
    local = ntohl(inet_addr(argv[2]));
    signal(SIGPIPE, SIG_IGN);
    setenv("DM_NET_BIND","1",1);
    setenv("DM_LDN_SCAN_MS","225",1);
    ensure(h_ldn_Init() == RES_OK,"initialize");
    ldn_usercfg user = {.user="NX reference"};
    ldn_seccfg security = {.security_mode=1};
    ldn_netcfg config = {.intent={.local_comm_id=0x0100000000abc000ULL},.node_count_max=4,.local_comm_version=42};
    int game = socket(AF_INET, SOCK_DGRAM, 0);
    int echo_peer = getenv("RC_ECHO_PEER") != NULL;
    struct sockaddr_in addr = {.sin_family=AF_INET,.sin_port=htons(echo_peer ? 5511 : 7788),.sin_addr.s_addr=htonl(local)};
    struct timeval timeout = {.tv_sec=12};
    ensure(game >= 0 && setsockopt(game,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout)==0 &&
           bind(game,(struct sockaddr*)&addr,sizeof addr)==0,"game socket");
    ldn_netinfo info = {0};
    if (host) {
        ensure(h_ldn_OpenAp()==RES_OK && h_ldn_Create(&config,&security,&user)==RES_OK,"host");
    } else {
        ensure(h_ldn_OpenSta()==RES_OK,"station");
        ldn_filter filter={.id.intent=config.intent,.flag=F_LOCAL_COMM_ID};
        int found=0;
        for(int i=0; i<48 && !found;i++) h_ldn_Scan(&info,&found,1,&filter,0);
        ensure(found && h_ldn_Connect(&info,&security,&user,42,0)==RES_OK,"discover and join");
    }
    for(int i=0;i<180;i++) {
        h_ldn_NetInfo(&info);
        if(info.ldn.node_count==2) break;
        usleep(100000);
    }
    ensure(info.ldn.node_count==2,"two nodes");
    uint32_t remote = info.ldn.nodes[host ? 1 : 0].ip;
    char message[32];
    socklen_t length=sizeof addr;
    if (echo_peer) {
        ensure(recvfrom(game,message,sizeof message,0,(struct sockaddr*)&addr,&length)==7 &&
               !memcmp(message,"nx-game",7) && ntohl(addr.sin_addr.s_addr)==remote,"peer UDP receive");
        ensure(sendto(game,"peer-ok",7,0,(struct sockaddr*)&addr,sizeof addr)==7,"peer UDP reply");
        puts("NX echo peer: exchanged data with another routed LDN client");
        h_ldn_Fini(); close(game); return 0;
    }
    addr.sin_port=htons(5511); addr.sin_addr.s_addr=htonl(remote);
    ensure(sendto(game,"nx-game",7,0,(struct sockaddr*)&addr,sizeof addr)==7,"game send");
    ensure(recvfrom(game,message,sizeof message,0,(struct sockaddr*)&addr,&length)==7 &&
           !memcmp(message,"peer-ok",7) && ntohl(addr.sin_addr.s_addr)==remote,"game reply");
    puts("NX reference: room admitted and bidirectional game UDP delivered");
    h_ldn_Fini();
    close(game);
    return 0;
}
