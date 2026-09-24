#ifndef FORGESCHED_PROTOCOL_MESSAGE_H
#define FORGESCHED_PROTOCOL_MESSAGE_H

#include "ProtocolVersion.h"
#include "MessageType.h"
#include "nlohmann/json.hpp"

namespace ForgeSched::Protocol {

struct ProtocolMessage {
    uint32_t version{PROTOCOL_VERSION};
    MessageType type{MessageType::UNKNOWN};
    uint64_t request_id{0};
    nlohmann::json data;
};

} // namespace ForgeSched::Protocol

#endif // FORGESCHED_PROTOCOL_MESSAGE_H
