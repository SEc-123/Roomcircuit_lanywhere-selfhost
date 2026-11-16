#!/usr/bin/env python3
"""Exercise the actual native node lookup with controlled Switch resolver boundaries."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / 'console/ldn_mitm/source'
SOURCE = (NATIVE / 'lanywhere_relay.cpp').read_text()
resolver = SOURCE[SOURCE.index('        struct NodeDnsWatch'):SOURCE.index('        bool ResolveDiscoveryRoute(')]

fixture = r'''
#include "node_catalog.h"
#include "node_lobby.h"
#include "selection_trace.hpp"
namespace selection_trace = ams::mitm::selection_trace;
#include <arpa/inet.h>
#include <netdb.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

using u32 = std::uint32_t;
using u8 = std::uint8_t;
extern "C" void *__libnx_alloc(std::size_t);
extern "C" void __libnx_free(void *);

struct Result {
    int value;
    Result(int v):value(v){}
    unsigned GetValue()const{return value;}
    operator int()const{return value;}
};
struct TimeSpan {
    unsigned milliseconds;
    static TimeSpan FromSeconds(unsigned s) { return {s * 1000}; }
    static TimeSpan FromMilliSeconds(unsigned ms) { return {ms}; }
    bool operator>=(TimeSpan other) const { return milliseconds >= other.milliseconds; }
};
namespace os {
    using SdkMutex = std::mutex;
    struct Tick {
        unsigned milliseconds;
        TimeSpan ToTimeSpan() const { return {milliseconds}; }
        Tick operator-(Tick other) const { return {milliseconds - other.milliseconds}; }
    };
    struct ThreadType {
        void (*run)(void *);
        void *arg;
    };
    unsigned now = 1, starts = 0, destroys = 0;
    bool can_start = true;
    Tick GetSystemTick() { return {now}; }
    Result CreateThread(ThreadType *t, void (*run)(void *), void *arg, void *, std::size_t, int) {
        if (!can_start) return 1;
        *t = {run, arg};
        return 0;
    }
    void SetThreadNamePointer(ThreadType *, const char *) {}
    void StartThread(ThreadType *) { ++starts; }
    void WaitThread(ThreadType *) {}
    void DestroyThread(ThreadType *t) { *t = {}; ++destroys; }
    void SleepThread(TimeSpan span) { now += span.milliseconds; }
}
#define R_FAILED(r) ((r) != 0)
static os::SdkMutex g_lock, g_node_resolve_lock;
static bool LoadServer(sockaddr_in *out) {
    out->sin_port = htons(11460);
    return ::inet_pton(AF_INET, "8.8.8.8", &out->sin_addr) == 1;
}
static bool CommunityEndpoint(sockaddr_in *out) {
    return ::inet_pton(AF_INET, "1.1.1.1", &out->sin_addr) == 1;
}

static size_t NodeSlot(u32 id) { return id < NODE_SHARED_CAPACITY ? id : id >= NODE_LOCAL_BASE && id < NODE_LOCAL_BASE + NODE_LOCAL_CAPACITY ? NODE_SHARED_CAPACITY + id - NODE_LOCAL_BASE : NODE_TOTAL_CAPACITY; }
static bool LookupNode(u32 id, NodeDynamicEntry *out) {
 if(id>=NODE_CATALOG_COUNT)return false;
 *out={};out->id=id;out->protocol=LanywhereNodes[id].protocol;out->port=LanywhereNodes[id].port;
 std::snprintf(out->host,sizeof(out->host),"%s",LanywhereNodes[id].host);return true;
}
static unsigned handle = 42, queries = 0, cancels = 0, frees = 0;
static constexpr unsigned PublicIpv4 = 0x09090909;
static bool watchdog = false, fail_dns = false, bad_addr = false;
static unsigned lookup_delay = 1, dns_ipv4 = PublicIpv4;
static unsigned resolverGetCancelHandle() { return handle; }
static int resolverCancel(unsigned cancel) {
    assert(cancel == handle);
    ++cancels;
    return 0;
}
static int test_getaddrinfo(const char *, const char *, const addrinfo *, addrinfo **);
static void test_freeaddrinfo(addrinfo *value) {
    ++frees;
    __libnx_free(value->ai_addr);
    __libnx_free(value);
}
#define getaddrinfo test_getaddrinfo
#define freeaddrinfo test_freeaddrinfo
''' + resolver + r'''
#undef getaddrinfo
#undef freeaddrinfo
static int test_getaddrinfo(const char *host, const char *service, const addrinfo *hints, addrinfo **out) {
    assert(!std::strcmp(host, LanywhereNodes[2].host) && !service && hints->ai_family == AF_INET);
    ++queries;
    os::now += lookup_delay;
    if (watchdog)
        g_node_dns_thread.run(g_node_dns_thread.arg);
    if (fail_dns)
        return EAI_AGAIN;
    *out = static_cast<addrinfo *>(__libnx_alloc(sizeof(addrinfo)));
    assert(*out);
    **out = {};
    sockaddr_in *answer = static_cast<sockaddr_in *>(__libnx_alloc(sizeof(sockaddr_in)));
    assert(answer);
    *answer = {};
    answer->sin_family = AF_INET;
    answer->sin_addr.s_addr = htonl(dns_ipv4);
    (*out)->ai_addr = reinterpret_cast<sockaddr *>(answer);
    (*out)->ai_family = AF_INET;
    (*out)->ai_addrlen = bad_addr ? 1 : sizeof(sockaddr_in);
    return 0;
}
int main() {
    __libnx_free(nullptr);
    sockaddr_in out{};
    assert(ResolveNodeEndpoint(0, &out) && out.sin_addr.s_addr == htonl(0x08080808));
    assert(ResolveNodeEndpoint(1, &out) && out.sin_addr.s_addr == htonl(0x01010101));
    in_addr numeric{};
    auto numeric_node = static_cast<u32>(NODE_CATALOG_COUNT - 1);
    assert(inet_pton(AF_INET, LanywhereNodes[numeric_node].host, &numeric) == 1);
    assert(ResolveNodeEndpoint(numeric_node, &out) && out.sin_addr.s_addr == numeric.s_addr);
    assert(queries == 0 && os::starts == 0);
    assert(ResolveNodeEndpoint(2, &out) && out.sin_addr.s_addr == htonl(PublicIpv4));
    assert(out.sin_port == htons(LanywhereNodes[2].port) && queries == 1 && frees == 1);
    fail_dns = true;
    assert(!ResolveNodeEndpoint(2, &out) && out.sin_addr.s_addr == htonl(PublicIpv4));
    fail_dns = false;
    bad_addr = true;
    assert(!ResolveNodeEndpoint(2, &out));
    bad_addr = false;
    dns_ipv4 = 0x7f000001;
    assert(!ResolveNodeEndpoint(2, &out));
    dns_ipv4 = PublicIpv4;
    lookup_delay = 3000;
    assert(!ResolveNodeEndpoint(2, &out) && cancels == 0);
    watchdog = true;
    assert(!ResolveNodeEndpoint(2, &out) && cancels == 1);
    watchdog = false;
    lookup_delay = 1;
    handle = 0;
    assert(!ResolveNodeEndpoint(2, &out) && queries == 6);
    handle = 42;
    os::can_start = false;
    assert(!ResolveNodeEndpoint(2, &out) && queries == 6 && frees == 5);
    assert(os::starts == 6 && os::destroys == 6);
    assert(!ResolveNodeEndpoint(NODE_CATALOG_COUNT, &out));
    assert(!ResolveNodeEndpoint(2, nullptr));
    std::puts("PASS native DNS ownership, hostname vs numeric route, failures and late cancellation");
}
'''

with tempfile.TemporaryDirectory(prefix='native-node-dns-') as temporary:
    directory = Path(temporary)
    source = directory / 'test.cpp'
    binary = directory / 'test'
    source.write_text(fixture)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
        '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + str(ROOT / 'common'), '-I' + str(NATIVE),
        str(source), str(NATIVE / 'libnx_allocator.cpp'), '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
