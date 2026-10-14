#ifndef GCRP_SCHEDULING_DRIVER_H
#define GCRP_SCHEDULING_DRIVER_H

#include <mutex>

namespace GCRP {

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

} // namespace GCRP

#endif // GCRP_SCHEDULING_DRIVER_H
