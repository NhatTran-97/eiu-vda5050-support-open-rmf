#ifndef LOOP_PACER_HPP
#define LOOP_PACER_HPP

#include <chrono>
#include <cstddef>

namespace vda5050_fleet_adapter_full_control::util {

// Schedules the passes of a loop that runs once per period at fixed times, whatever each pass takes.
class LoopPacer
{
public:
    // The first pass starts at `start`; a period that is not positive becomes one millisecond.
    LoopPacer(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::duration period);

    // When the next pass starts after one that ended at `now`; missed slots are skipped.
    std::chrono::steady_clock::time_point next(std::chrono::steady_clock::time_point now);

    // Number of passes that ended after the start of the next slot.
    std::size_t overruns() const { return _overruns; }

private:
    std::chrono::steady_clock::time_point _slot;
    std::chrono::steady_clock::duration _period;
    std::size_t _overruns = 0;
};

}  // namespace vda5050_fleet_adapter_full_control::util

#endif  // LOOP_PACER_HPP
