#pragma once
#include <stddef.h>
#include <stdint.h>

#define UPDATE_FORMAT_VERSION 1u
#define UPDATE_MANIFEST_SIZE 512u
#define UPDATE_MANIFEST_BODY_SIZE 448u
#define UPDATE_MAX_COMPRESSED (8u * 1024u * 1024u)
#define UPDATE_MAX_RAW (16u * 1024u * 1024u)
#define UPDATE_FILE_COUNT 22u
#define UPDATE_CHUNK_SIZE 32768u

/* The IDs are stable install destinations. The module is installed last. */
static const char *const LanywhereUpdateFiles[UPDATE_FILE_COUNT] = {
    "lanywhere/INSTALL.zh-CN.txt",
    "lanywhere/licenses/Atmosphere-libs.txt",
    "lanywhere/licenses/LANywhere.txt",
    "lanywhere/licenses/TitleDB-MIT.txt",
    "lanywhere/third_party/README.md",
    "lanywhere/third_party/licenses/harfbuzz-COPYING",
    "lanywhere/third_party/licenses/libjpeg-turbo-README.ijg",
    "lanywhere/third_party/licenses/libnx-LICENSE.md",
    "lanywhere/third_party/licenses/libtesla-LICENSE.txt",
    "lanywhere/third_party/licenses/switch-bzip2/LICENSE",
    "lanywhere/third_party/licenses/switch-freetype/FTL.TXT",
    "lanywhere/third_party/licenses/switch-freetype/GPLv2.TXT",
    "lanywhere/third_party/licenses/switch-freetype/LICENSE.TXT",
    "lanywhere/third_party/licenses/switch-libjpeg-turbo/LICENSE.md",
    "lanywhere/third_party/licenses/switch-libpng/LICENSE",
    "lanywhere/third_party/licenses/switch-zlib/LICENSE",
    "lanywhere/third_party/licenses/libsodium-LICENSE",
    "atmosphere/contents/4200000000000010/toolbox.json",
    "switch/.overlays/lanywhere.ovl",
    "switch/lanywhere.nro",
    "atmosphere/contents/4200000000000010/flags/boot2.flag",
    "atmosphere/contents/4200000000000010/exefs.nsp",
};
