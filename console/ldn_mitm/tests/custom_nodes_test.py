#!/usr/bin/env python3
"""Exercise the actual shared catalog and local INI parsers."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = r'''
#include "custom_nodes.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace ams::mitm::custom_nodes;
static void put32(unsigned char *p, unsigned v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
int main() {
    unsigned char hash[32] = {3}, packet[40 + 192] = {};
    std::memcpy(packet,hash,32); packet[32]=1;
    put32(packet+36,1u<<13); put32(packet+40,13);
    packet[44]=2; packet[45]=2; packet[48]=0xbb; packet[49]=0x2c; packet[50]=1;
    std::strcpy(reinterpret_cast<char *>(packet+52),"example.net");
    std::strcpy(reinterpret_cast<char *>(packet+180),"Europe");
    packet[228]=8;packet[229]=8;packet[230]=8;packet[231]=8;
    NodeDynamicCatalog shared{};
    assert(ParseShared(packet,sizeof(packet),hash,&shared));
    assert(shared.count==1 && shared.nodes[0].id==13 && shared.nodes[0].port==11451);
    assert(shared.available==(1u<<13));
    NodeDynamicCatalog unchanged=shared;
    packet[51]=1;
    assert(!ParseShared(packet,sizeof(packet),hash,&shared));
    assert(!std::memcmp(&shared,&unchanged,sizeof(shared)));
    packet[51]=0;
    unsigned char labelled[sizeof(packet)+NODE_REGION_LABEL_BYTES]{};
    std::memcpy(labelled,packet,sizeof(packet));
    labelled[33]=NODE_REGION_LABEL_VERSION;
    std::strcpy(reinterpret_cast<char *>(labelled+sizeof(packet)),"São Paulo");
    assert(ParseShared(labelled,sizeof(labelled),hash,&shared));
    assert(!std::strcmp(shared.region_labels[node_region_label_index(13)],"São Paulo"));
    assert(!ParseShared(labelled,sizeof(labelled)-1,hash,&shared));
    const char *conf = "# local\n[node]\nname=Test\nhost=LOCAL.example.\nport=11451\nregion=europe\n"
                       "[node]\nname=Duplicate central\nhost=EXAMPLE.NET\nport=11451\n";
    NodeDynamicCatalog result{};
    assert(ParseLocal(conf,std::strlen(conf),shared,&result));
    assert(result.count==2 && result.nodes[1].id==NODE_LOCAL_BASE);
    assert(!std::strcmp(result.nodes[1].host,"local.example"));
    assert(result.nodes[1].region==2 && result.nodes[1].port==11451);
    assert(!std::strcmp(result.region_labels[node_region_label_index(13)],"São Paulo"));
    const char *china = "[node]\nname=Beijing\nhost=china.example.net\nport=11451\nregion=中国\n";
    assert(ParseLocal(china,std::strlen(china),shared,&result));
    assert(result.nodes[1].region==1 &&
           !std::strcmp(result.region_labels[node_region_label_index(NODE_LOCAL_BASE)],"China"));
    const char *bad[] = {"[node]\nname=X\nhost=x\nport=0\n", "[node]\nname=X\nhost=bad/name\nport=2\n",
                         "[node]\nname=X\nhost=x\nport=65536\n", "[node]\nname=X\nhost=x\nport=2\ninvalid=3\n",
                         "[node]\nname=X\nhost=x\nport=2\nport=3\n"};
    for (const auto *s : bad) {
        auto saved=result;
        assert(!ParseLocal(s,std::strlen(s),shared,&result));
        assert(!std::memcmp(&result,&saved,sizeof(result)));
    }
    char near_limit[8192]; std::memset(near_limit, '#', sizeof(near_limit));
    near_limit[sizeof(near_limit)-1] = '\n';
    assert(ParseLocal(near_limit,sizeof(near_limit),shared,&result));
    assert(result.count == shared.count);
    assert(!ParseShared(packet,sizeof(packet)-1,hash,&result));
    assert(!ParseLocal(nullptr,8193,shared,&result));
    puts("PASS shared wire validation, local INI, endpoint deduplication and atomic failures");
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-nodes-') as directory:
    path = Path(directory)
    (path/'test.cpp').write_text(source)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17','-Wall','-Wextra','-Werror',
        '-I'+str(root/'console/ldn_mitm/source'),str(path/'test.cpp'),
        str(root/'console/ldn_mitm/source/custom_nodes.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=10)
