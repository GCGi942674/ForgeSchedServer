#ifndef FORGESCHED_PROTOCOL_ERROR_H
#define FORGESCHED_PROTOCOL_ERROR_H

#include <string>

namespace ForgeSched::Protocol {

enum class ProtocolErrorCode {
    NONE,
    INVALID_JSON,
    MISSING_FIELD,
    INVALID_FIELD_TYPE,
    INVALID_FIELD_VALUE,
    UNSUPPORTED_VERSION,
    UNKNOWN_MESSAGE_TYPE
};

struct ProtocolError {
    ProtocolErrorCode code{ProtocolErrorCode::NONE};
    std::string message;

    bool hasError() const { return code != ProtocolErrorCode::NONE; }
};

} // namespace ForgeSched::Protocol

#endif // FORGESCHED_PROTOCOL_ERROR_H
