#ifndef FORGESCHED_WORKER_DISPATCHER_H
#define FORGESCHED_WORKER_DISPATCHER_H

#include "task/Task.h"
#include <string>

namespace ForgeSched {

using WorkerId = std::string;

class WorkerDispatcher {
public:
    virtual ~WorkerDispatcher() = default;

    virtual bool dispatch(
        const WorkerId& worker_id,
        const Task& task
    ) = 0;
};

} // namespace ForgeSched

#endif // FORGESCHED_WORKER_DISPATCHER_H
