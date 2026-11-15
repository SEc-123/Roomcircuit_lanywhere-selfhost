#include "selection_trace.hpp"
#if defined(__SWITCH__)
#include <stratosphere.hpp>
#include <algorithm>
#include <cerrno>
#include <cstring>

namespace ams::mitm::selection_trace {
namespace {
constexpr const char Path[] = "sdmc:/lanywhere/node-selection.log";
constexpr s64 MaxBytes = 8192;
os::SdkMutex g_trace_lock;
Handle g_operation = 0;
std::uint32_t g_node = 0;
unsigned g_sequence = 0;
s64 g_offset = 0;
bool g_writable = false;

class RestoreState {
    u8 saved[0x100];
    int saved_errno;
public:
    RestoreState() : saved_errno(errno) { std::memcpy(saved, armGetTls(), sizeof(saved)); }
    ~RestoreState() {
        std::memcpy(armGetTls(), saved, sizeof(saved));
        errno = saved_errno;
    }
};

bool Append(const char *stage, std::uint32_t result) {
    char line[192];
    const int count = util::TSNPrintf(line, sizeof(line),
        "format=1 build=node-select-20261116b module=%s node=%u seq=%u stage=%s result=%08x\n",
        GITDESCVER, g_node, g_sequence, stage, result);
    if (count <= 0 || static_cast<size_t>(count) >= sizeof(line) ||
        g_offset > MaxBytes - count) return false;
    fs::FileHandle file;
    if (R_FAILED(fs::OpenFile(&file, Path, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) return false;
    const Result written = fs::WriteFile(file, g_offset, line, static_cast<size_t>(count), fs::WriteOption::Flush);
    fs::CloseFile(file);
    if (R_FAILED(written)) return false;
    g_offset += count;
    ++g_sequence;
    return true;
}
} // namespace

Handle Begin(std::uint32_t node, const char *stage) {
    RestoreState restore;
    std::scoped_lock lock(g_trace_lock);
    do { ++g_operation; } while (!g_operation);
    g_node = node;
    g_sequence = 0;
    g_offset = 0;
    g_writable = false;
    static_cast<void>(fs::CreateDirectory("sdmc:/lanywhere"));
    bool exists = false;
    if (R_FAILED(fs::HasFile(&exists, Path))) return g_operation;
    if (!exists && R_FAILED(fs::CreateFile(Path, 0))) return g_operation;
    fs::FileHandle file;
    if (R_FAILED(fs::OpenFile(&file, Path, fs::OpenMode_Write))) return g_operation;
    const Result cleared = fs::SetFileSize(file, 0);
    fs::CloseFile(file);
    if (R_SUCCEEDED(cleared)) g_writable = Append(stage, 0);
    return g_operation;
}

void Record(Handle operation, const char *stage, std::uint32_t result) {
    RestoreState restore;
    std::scoped_lock lock(g_trace_lock);
    if (!g_writable || !operation || operation != g_operation) return;
    g_writable = Append(stage, result);
}
} // namespace ams::mitm::selection_trace
#endif
