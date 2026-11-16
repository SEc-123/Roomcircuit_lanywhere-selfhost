# RoomCircuit Network Probe

**Development only:** The endpoint and embedded CA below are tied to one test environment and time-limited. Do not use this probe as a production or third-party configuration.

This NRO uses the HOS SSL service on HOS 19.0.1, emuMMC type 1. It connects only to the independent service at `https://49.233.147.207/native/compatibility`. The positive case uses the embedded CA in a private SSL context. Two fresh contexts check rejection with the wrong hostname and without that CA. Hostname, certificate and date verification stay enabled in all three cases. A timeout or an unrelated network error does not count as a passing negative check.

The only persistent output is `sd:/switch/roomcircuit-native/network-probe.txt`. The context trust and session state are temporary. Startup checks emuMMC type before querying the HOS version and checks both before mounting SD or initializing the display, sockets and SSL. It exits outside this target. No account service or system-storage mount is used.

Build from `native-validation` with the published devkitPro image and the public own CA DER in `https-endpoint/ca.der`.

```sh
docker run --rm --network none -v "$PWD:/validation" -w /validation/console-network \
  devkitpro/devkita64:20260219 make CA_DER=../https-endpoint/ca.der
```

On the confirmed emuMMC, open **RoomCircuit Network Probe** from Album. A passing result requires HTTP 200 with the own CA, SSL verification result `0x25A7B` for the wrong hostname and `0x2687B` without the own CA. A different TLS failure leaves the expected cause unconfirmed. These result names appear in the [SwitchBrew SSL result table](https://switchbrew.org/wiki/Error_codes). Retail account and game login require a separate console check.

After the TLS cases, the probe asks the active virtual SSL service whether it can register its existing internal client identity in a temporary context. It sends no connection using that identity and exports no key or certificate. Success is an API availability result, not proof of native device authentication.
