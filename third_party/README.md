# Included libraries and source references

The original LANywhere/ldn_mitm GPL-2.0 notice is preserved in the project root.
Atmosphere-libs and libtesla source, with their licenses, is included under `console/`.

The front end links the devkitPro builds of FreeType, HarfBuzz, libpng, bzip2, zlib,
libjpeg-turbo and libnx, together with the devkitA64 runtime. Packaged license notices
are retained in `licenses/`. FreeType is included under its GPLv2 option. The upstream
projects and exact package versions/hashes used for the build are recorded below and
in `scripts/toolchain/packages.lock.json`.

- Fork protocol reference: https://github.com/jieluojun/ldn_mitm at commit `1014c70d5f82b19bf774b00c851c237d805531ae`; its repository provides GPLv2 terms and documents exemptions for specific named projects. See `UPSTREAM.json`. The submitted archive's binary has not been reproduced from that source.
- libnx source revision is pinned in `UPSTREAM.json`: https://github.com/switchbrew/libnx
- devkitPro package build recipes: https://github.com/devkitPro/portlibs
- FreeType 2.13.3: https://gitlab.freedesktop.org/freetype/freetype
- HarfBuzz 10.0.1: https://github.com/harfbuzz/harfbuzz
- libpng 1.6.48: https://github.com/pnggroup/libpng
- bzip2 1.0.8: https://sourceware.org/bzip2/
- zlib 1.3.1: https://github.com/madler/zlib
- libjpeg-turbo 2.1.2: https://github.com/libjpeg-turbo/libjpeg-turbo
- devkitA64/GCC runtime build sources: https://github.com/devkitPro/buildscripts

This folder contains the notices supplied with the fetched SDK packages; it is not
a claim that the original console games, Nintendo system software, or their assets
are included. No games, ROMs, console keys or user credentials are in this delivery.

The supplied fork's Chinese title database and nx-ovlloader heap settings were not imported.

- libsodium 1.0.20 verifies Ed25519 update signatures. ISC license in `licenses/libsodium-LICENSE`.

## lwIP 2.2.1

Source is included in `console/lanplay-lwip`.

```text
Copyright (c) 2001, 2002 Swedish Institute of Computer Science.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.
```
