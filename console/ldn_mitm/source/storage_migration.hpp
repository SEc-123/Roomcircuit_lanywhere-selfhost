#pragma once
#include <cstdint>

namespace ams::mitm::storage {
enum class Selection : std::uint32_t { Legacy = 0, Current = 1, Unavailable = 2 };
// Called once after SD mount, before any RoomCircuit service starts.
bool Initialize();
Selection StorageSelection();
bool StorageUseCurrent();
bool StorageAvailable();
}
