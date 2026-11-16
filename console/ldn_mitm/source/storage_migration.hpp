#pragma once
#include <cstdint>

namespace ams::mitm::storage {
enum class Selection : std::uint32_t { Legacy = 0, V1 = 1, Unavailable = 2, Current = 3 };
// Called once after SD mount, before any RoomCircuit service starts.
bool Initialize();
Selection StorageSelection();
bool StorageUseCurrent();
unsigned StorageGeneration();
unsigned StorageHealth();
bool StorageAvailable();
}
