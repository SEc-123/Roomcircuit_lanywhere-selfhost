#include <cstdlib>

// libnx DNS uses these hooks. This sysmodule already owns a bounded heap
// initialized by ams::init::Startup before it accepts IPC requests.
extern "C" void *__libnx_alloc(std::size_t size) {
    return std::malloc(size);
}

extern "C" void __libnx_free(void *allocation) {
    std::free(allocation);
}
