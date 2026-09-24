#ifndef FORGESCHED_WORKER_STATUS_H
#define FORGESCHED_WORKER_STATUS_H

#include <string>

namespace ForgeSched {

enum class WorkerStatus {
    ONLINE,
    OFFLINE
};

std::string toString(WorkerStatus status);

} // namespace ForgeSched

#endif // FORGESCHED_WORKER_STATUS_H
