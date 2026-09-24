#ifndef FORGESCHED_SCHEDULING_DRIVER_H
#define FORGESCHED_SCHEDULING_DRIVER_H

#include <mutex>

namespace ForgeSched {

class SchedulingCoordinator;

class SchedulingDriver {
public:
    explicit SchedulingDriver(SchedulingCoordinator& coordinator);
    ~SchedulingDriver() = default;

    SchedulingDriver(const SchedulingDriver&) = delete;
    SchedulingDriver& operator=(const SchedulingDriver&) = delete;

    void requestSchedule();

private:
    SchedulingCoordinator& coordinator_;
    std::mutex mutex_;
    // Protected together, including the transition back to idle.
    bool running_{false};
    bool pending_{false};
};

} // namespace ForgeSched

#endif // FORGESCHED_SCHEDULING_DRIVER_H
