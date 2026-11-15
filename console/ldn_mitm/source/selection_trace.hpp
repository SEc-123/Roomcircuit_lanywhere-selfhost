#pragma once
#include <cstdint>

namespace ams::mitm::selection_trace {
using Handle = std::uint64_t;

// The latest selection is retained even when no debug flag is installed. This
// records the failing boundary; it does not establish a cause for a hard freeze.
#if defined(__SWITCH__)
Handle Begin(std::uint32_t node, const char *stage);
void Record(Handle operation, const char *stage, std::uint32_t result = 0);
#else
inline Handle Begin(std::uint32_t, const char *) { return 0; }
inline void Record(Handle, const char *, std::uint32_t = 0) {}
#endif
} // namespace ams::mitm::selection_trace
