#!/bin/sh
set -eu
stage=/tmp/roomcircuit-native-lab-a54af14c29
base="$stage/console-tls"
umask 077
mkdir -p "$base"
if [ ! -f "$base/server.pem" ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 30 -keyout "$base/ca-key.pem" -out "$base/ca.pem" -subj '/CN=RoomCircuit Native Test CA' -addext 'basicConstraints=critical,CA:TRUE,pathlen:0' -addext 'keyUsage=critical,keyCertSign,cRLSign' -addext 'nameConstraints=critical,permitted;IP:49.233.147.207/255.255.255.255,permitted;DNS:roomcircuit.native' > "$base/build.log" 2>&1
    openssl req -newkey rsa:2048 -nodes -keyout "$base/server-key.pem" -out "$base/server.csr" -subj '/CN=roomcircuit.native' >> "$base/build.log" 2>&1
    printf '%s\n' 'basicConstraints=critical,CA:FALSE' 'subjectAltName=IP:49.233.147.207,DNS:roomcircuit.native' 'extendedKeyUsage=serverAuth' 'keyUsage=digitalSignature,keyEncipherment' > "$base/leaf.ext"
    openssl x509 -req -in "$base/server.csr" -CA "$base/ca.pem" -CAkey "$base/ca-key.pem" -CAcreateserial -days 14 -extfile "$base/leaf.ext" -out "$base/server.pem" >> "$base/build.log" 2>&1
fi
openssl verify -CAfile "$base/ca.pem" -verify_ip 49.233.147.207 "$base/server.pem"
chown nobody:nobody "$base/server.pem" "$base/server-key.pem"
chmod 644 "$base/server.pem"
chmod 600 "$base/server-key.pem"
chmod 711 "$base" "$stage"
systemd-run --unit=roomcircuit-native-console-tls --property=User=nobody --property=Group=nobody --property=MemoryMax=48M --property=CPUQuota=30% --property=NoNewPrivileges=yes --property=AmbientCapabilities=CAP_NET_BIND_SERVICE --property=CapabilityBoundingSet=CAP_NET_BIND_SERVICE --setenv=RC_TLS_CERT="$base/server.pem" --setenv=RC_TLS_KEY="$base/server-key.pem" python3 "$stage/server.py"
