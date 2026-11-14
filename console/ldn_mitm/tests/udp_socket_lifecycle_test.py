#!/usr/bin/env python3
"""Run the native UDP send/close race with a blocked syscall boundary."""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root / 'source/lanywhere_relay.cpp')
args = parser.parse_args()
source = args.source.read_text()


def function(signature):
    start = source.index(signature)
    depth = 0
    for at in range(source.index('{', start), len(source)):
        if source[at] == '{':
            depth += 1
        elif source[at] == '}':
            depth -= 1
            if depth == 0:
                return source[start:at + 1]
    raise ValueError(signature)


harness = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
using u8=uint8_t; using u32=uint32_t; using u64=uint64_t;
static std::mutex g_send_lock, seam_lock;
static std::condition_variable seam_event;
static int g_tcp=10,g_udp=11,g_server=1;
static u32 g_token=123;
static std::atomic<u64>g_stream_epoch{1};
static std::atomic<bool>g_stream_ready{true};
static bool sending=false,release_send=false,closed=false,closing=false;
namespace tcpstream { void ResetTransport(u64) {} }
static int MockClose(int fd) {
    if(fd==11) {
        std::scoped_lock lock(seam_lock); closed=true; seam_event.notify_all();
    }
    return 0;
}
static bool UdpSendTo(int fd,int endpoint,u32 token,u8,const void*,size_t) {
    assert(fd==11 && endpoint==1 && token==123);
    std::unique_lock lock(seam_lock); sending=true; seam_event.notify_all();
    seam_event.wait(lock,[]{return release_send;});
    return true;
}
#define close MockClose
/* SOURCE */
#undef close
int main() {
    std::thread sender([]{ assert(UdpSend(1,nullptr,0)); });
    { std::unique_lock lock(seam_lock); seam_event.wait(lock,[]{return sending;}); }
    std::thread closer([]{
        { std::scoped_lock lock(seam_lock); closing=true; seam_event.notify_all(); }
        CloseSockets();
    });
    bool overlapped;
    {
        std::unique_lock lock(seam_lock);
        seam_event.wait(lock,[]{return closing;});
        seam_event.wait_for(lock,std::chrono::milliseconds(100),[]{return closed;});
        overlapped=closed;
        release_send=true; seam_event.notify_all();
    }
    sender.join(); closer.join();
    assert(!overlapped && closed && g_udp==-1 && g_tcp==-1 && !g_stream_ready);
    puts("PASS: native UDP send completes before recovery closes or replaces its socket");
}
'''.replace('/* SOURCE */', function('        bool UdpSend(') + '\n' + function('        void CloseSockets('))
with tempfile.TemporaryDirectory(prefix='udp-socket-lifecycle-') as temporary:
    directory = Path(temporary)
    cpp, executable = directory / 'test.cpp', directory / 'test'
    cpp.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
        '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', str(cpp), '-o', str(executable)], check=True, timeout=30)
    subprocess.run([str(executable)], check=True, timeout=10)
print('Actual native SHA-256', hashlib.sha256(source.encode()).hexdigest())
