#!/usr/bin/env python3
"""Bounded updater requests against real loopback peers, including partial data and cancellation."""
from pathlib import Path
import os
import socket
import struct
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
DRIVER = r'''
#include "update_network.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static atomic_bool cancel;
static bool stop_download;
static enum { FileOk, FailFlush, FailSync, FailClose } write_fault;
static bool file_closed;
static unsigned last_received, full_progress;

static int test_fflush(FILE *file) {
    if (write_fault == FailFlush) { errno = EIO; return EOF; }
    return fflush(file);
}
static int test_fsync(int fd) {
    if (write_fault == FailSync) { errno = EIO; return -1; }
    return fsync(fd);
}
static int test_fclose(FILE *file) {
    const int result = fclose(file);
    file_closed = true;
    if (write_fault == FailClose) { errno = EIO; return EOF; }
    return result;
}

#define fflush test_fflush
#define fsync test_fsync
#define fclose test_fclose
#include "update_network.c"
#undef fclose
#undef fsync
#undef fflush

static void progress(uint32_t received,uint32_t total,void *unused) {
    (void)unused;
    assert(received >= last_received && received <= total);
    last_received = received;
    if (received == total) {
        assert(file_closed && write_fault == FileOk);
        full_progress++;
    }
    if(stop_download && received)atomic_store(&cancel,true);
}
int main(int argc,char **argv) {
    assert(argc==6);
    const char *mode=argv[1];int port=atoi(argv[2]);
    const char *destination=argv[3];unsigned size=(unsigned)atoi(argv[4]);
    UpdateNetworkResult result;
    if(!strcmp(mode,"manifest")) {
        uint8_t manifest[512];result=update_network_manifest("127.0.0.1",port,manifest,&cancel);
        if(result==UpdateNetworkOk)for(unsigned i=0;i<512;i++)assert(manifest[i]==(i&255));
    } else {
        uint8_t hash[32];memset(hash,0x5a,sizeof(hash));stop_download=!strcmp(mode,"cancel");
        if (!strcmp(mode, "flush_error")) write_fault = FailFlush;
        if (!strcmp(mode, "sync_error")) write_fault = FailSync;
        if (!strcmp(mode, "close_error")) write_fault = FailClose;
        result=update_network_download("127.0.0.1",port,hash,size,destination,&cancel,progress,NULL);
        assert(full_progress == (result == UpdateNetworkOk ? 1u : 0u));
        if (write_fault != FileOk) assert(file_closed && last_received > 0 && last_received < size);
    }
    printf("%d\n",result);
    assert((int)result==atoi(argv[5]));
}
'''


def receive(sock, size):
    data = b''
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise EOFError
        data += part
    return data


def frame(kind, body):
    return struct.pack('<HB', 1 + len(body), kind) + body


def exercise(binary, folder, name, mode, expected, behavior, size=65539):
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    listener.settimeout(8)
    errors = []

    def peer():
        try:
            with listener.accept()[0] as sock:
                sock.settimeout(8)
                while True:
                    try:
                        length, = struct.unpack('<H', receive(sock, 2))
                        request = receive(sock, length)
                    except EOFError:
                        return
                    if behavior == 'eof':
                        return
                    if behavior == 'timeout':
                        time.sleep(6.5)
                        return
                    if behavior == 'busy':
                        sock.sendall(frame(0x85, b'\7busy'))
                        return
                    if mode == 'manifest':
                        assert request == b'\x30'
                        payload = frame(0xb0, bytes(range(256)) * 2)
                        if behavior == 'oversize':
                            payload = struct.pack('<HB', 514, 0xb0)
                        for offset in range(0, len(payload), 11):
                            sock.sendall(payload[offset:offset + 11])
                        return
                    assert request[:33] == b'\x31' + bytes([0x5a]) * 32
                    offset, = struct.unpack_from('<I', request, 33)
                    count = min(32768, size - offset)
                    wire_offset = offset + 1 if behavior == 'wrong_offset' else offset
                    payload = frame(0xb1, struct.pack('<I', wire_offset) + bytes((i % 251 for i in range(offset, offset + count))))
                    for start in range(0, len(payload), 1009):
                        sock.sendall(payload[start:start + 1009])
        except (ConnectionResetError, BrokenPipeError):
            pass
        except Exception as error:
            errors.append(error)
        finally:
            listener.close()

    thread = threading.Thread(target=peer)
    thread.start()
    target = folder / (name + '.bin')
    subprocess.run([str(binary), mode, str(listener.getsockname()[1]), str(target), str(size), str(expected)], check=True, timeout=10)
    thread.join(timeout=8)
    assert not thread.is_alive() and not errors, errors
    if mode != 'manifest':
        if expected == 0:
            assert target.read_bytes() == bytes(i % 251 for i in range(size))
        else:
            assert not target.exists()
    print('PASS', name)


with tempfile.TemporaryDirectory(prefix='update-network-') as temporary:
    directory = Path(temporary)
    (directory / 'driver.c').write_text(DRIVER)
    binary = directory / 'driver'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'source'), str(directory / 'driver.c'), '-o', str(binary)], check=True)
    exercise(binary, directory, 'partial manifest', 'manifest', 0, 'valid')
    exercise(binary, directory, 'complete chunks', 'download', 0, 'valid')
    exercise(binary, directory, 'flush failure with all chunks received', 'flush_error', 5, 'valid')
    exercise(binary, directory, 'sync failure with all chunks received', 'sync_error', 5, 'valid')
    exercise(binary, directory, 'close failure with all chunks received', 'close_error', 5, 'valid')
    exercise(binary, directory, 'cancel at a chunk', 'cancel', 1, 'valid')
    exercise(binary, directory, 'offset mismatch', 'download', 4, 'wrong_offset')
    exercise(binary, directory, 'bounded manifest', 'manifest', 4, 'oversize')
    exercise(binary, directory, 'node busy', 'manifest', 3, 'busy')
    exercise(binary, directory, 'peer closes', 'manifest', 2, 'eof')
    exercise(binary, directory, 'read timeout', 'manifest', 2, 'timeout')
