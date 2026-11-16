#pragma once
#include "../../../common/node_lobby.h"
namespace ams::mitm::lanywhere {
// The caller must keep the selected entry stable while writing. A false return
// leaves the previous validated file in place and must not commit the new route.
bool SaveNodePreference(const NodeDynamicEntry &entry);
enum class NodePreferenceRead { Missing, Valid, Invalid };
NodePreferenceRead ReadNodePreference(NodeDynamicEntry *entry);
}
