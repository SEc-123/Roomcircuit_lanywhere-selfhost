# RoomCircuit System Probe

This system inspector uses libnx to read the active emuMMC's public certificate metadata. It records firmware version, CertStore layout and certificate hashes in `sd:/switch/roomcircuit-native/probe.txt`. Startup requires emuMMC type 1 and HOS 19.0.1 before mounting SD or reading the system data archive.

BrowserDll certificate bundle checks report only candidate paths, sizes and hashes. The profile check records only the number of local profiles. Bundle presence by itself does not establish the Web Applet's active trust policy.

Launch the NRO through the Homebrew Menu in the existing emuMMC. Wait for `Done`, then press + to exit. Preserve the report for the HOS 19.0.1 network-client adaptation.

Based on the AGPL-3.0-only [OpenPak certificate probe](https://github.com/openpak/switch-patcher/tree/bf8f220f26a04e36a19b384ebfb101bde5be333d/tools/cert-probe), with bounded inspection of the public system data archive.
