#!/usr/bin/env python3
"""Prepare local native probe build inputs from an explicit owned DER CA."""
import argparse
import hashlib
import ipaddress
from pathlib import Path
import re
import subprocess
import sys
import ssl
import time


def write_if_changed(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data:
        temp = path.with_name(path.name + '.tmp')
        temp.write_bytes(data)
        temp.replace(path)


def prepare(ca_path, build, address=None):
    ca = Path(ca_path).read_bytes()
    if not 1 <= len(ca) <= 8192:
        raise ValueError('CA DER size must be 1–8192 bytes')
    cert = subprocess.run(['openssl','x509','-inform','DER','-noout','-checkend','60'],
                          input=ca, capture_output=True)
    if cert.returncode:
        raise ValueError('CA must be a valid X.509 DER certificate with at least 60 seconds remaining')
    basic = subprocess.run(['openssl','x509','-inform','DER','-noout','-text','-startdate'],
                           input=ca,capture_output=True)
    if basic.returncode or not re.search(rb'X509v3 Basic Constraints[^\n]*\n\s*CA\s*:\s*TRUE\b', basic.stdout):
        raise ValueError('certificate must explicitly declare CA:TRUE')
    dates = re.search(rb'(?m)^notBefore=(.+)$', basic.stdout)
    if not dates or ssl.cert_time_to_seconds(dates.group(1).decode('ascii')) > time.time():
        raise ValueError('CA certificate is not yet valid')
    endpoint = None
    if address is not None:
        ip = ipaddress.IPv4Address(address)
        if ip.is_unspecified or ip.is_loopback or ip.is_multicast or ip.is_link_local or ip.is_reserved:
            raise ValueError('probe target must be a usable non-local IPv4 address')
        endpoint = str(ip)
    digest = hashlib.sha256(ca).hexdigest()
    header = '#pragma once\n#define RC_NATIVE_CA_SHA256 "' + digest + '"\n'
    if endpoint:
        header += '#define RC_PROBE_HOST "' + endpoint + '"\n'
    build = Path(build)
    write_if_changed(build/'rc_build_config.h', header.encode())
    write_if_changed(build/'probe_ca.der', ca)
    write_if_changed(build/'romfs'/'ca.der', ca)
    return digest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ca', required=True)
    p.add_argument('--build-dir', required=True)
    p.add_argument('--address')
    args = p.parse_args()
    try:
        print('CA SHA256:',prepare(args.ca,args.build_dir,args.address))
    except (ValueError, OSError) as exc:
        p.error(str(exc))

if __name__ == '__main__':
    main()
