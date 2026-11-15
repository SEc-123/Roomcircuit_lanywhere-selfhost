#include "ldn_types.hpp"
#include <cstring>
#include <algorithm>

namespace ams::mitm::ldn {
    void NetworkInfo2NetworkConfig(NetworkInfo* info, NetworkConfig* out) {
        *out = {};
        out->intentId = info->networkId.intentId;
        out->channel = info->common.channel;
        out->nodeCountMax = info->ldn.nodeCountMax;
        out->localCommunicationVersion = info->ldn.nodes[0].localCommunicationVersion;
    }

    void NetworkInfo2SecurityParameter(NetworkInfo* info, SecurityParameter* out) {
        out->sessionId = info->networkId.sessionId;
        std::memcpy(out->unkRandom, info->ldn.unkRandom, 16);
    }

    bool MacAddress::operator==(const MacAddress& b) const {
        return std::memcmp(this->raw, b.raw, sizeof(MacAddress)) == 0;
    }

    bool Ssid::operator==(const Ssid& b) const {
        return this->length <= SsidLengthMax && b.length <= SsidLengthMax && (this->length == b.length) && (std::memcmp(this->raw, b.raw, this->length) == 0);
    }

    Ssid& Ssid::operator=(const char* s) {
        const size_t length = std::min(std::strlen(s), sizeof(this->raw) - 1);
        std::memcpy(this->raw, s, length);
        this->raw[length] = 0;
        this->length = static_cast<u8>(length);
        return *this;
    }

    bool SessionId::operator==(const SessionId& b) const {
        return (this->low == b.low) && (this->high == b.high);
    }
}
