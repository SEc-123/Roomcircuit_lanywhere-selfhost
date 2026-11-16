# Target-host loopback verification

Extract the archive into the isolated stage that already contains the account lab. The account lab's private `.runtime/lab.env` stays at its existing path. Stop the earlier test broker on ports 39280 and 39281 before starting the updated binary.

```sh
native="$stage/native-backend"
chmod 700 "$native/.bin/native-backend-durable-linux-amd64"
systemd-run --unit=roomcircuit-native-device-lab \
  --property=WorkingDirectory="$native" --property=MemoryMax=96M \
  --property=NoNewPrivileges=yes \
  --setenv=RC_DEVICE_BINARY="$native/.bin/native-backend-durable-linux-amd64" \
  --setenv=RC_ACCOUNT_LAB_ENV="$stage/.runtime/lab.env" \
  --setenv=RC_ACCOUNT_CORE_GRPC=127.0.0.1:20001 \
  /bin/sh "$native/run.sh"
python3 -B "$native/smoke.py"
```

The service reuses its TLS CA, RSA signing key, game-service key and device grants in `$native/.runtime/`. The smoke test verifies the CA and writes owner-only browser and game fixtures. `local-receipt.json` contains non-sensitive results. Game-runner fixture tokens expire after 15 minutes. Keep private state and keys in this stage when updating the binary.
