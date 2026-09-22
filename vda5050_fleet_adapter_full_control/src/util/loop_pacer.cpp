#include "vda5050_fleet_adapter_full_control/util/loop_pacer.hpp"

namespace vda5050_fleet_adapter_full_control::util {

LoopPacer::LoopPacer(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::duration period)
    : _slot(start), _period(period > std::chrono::steady_clock::duration::zero() ? period : std::chrono::milliseconds(1))
{
}

std::chrono::steady_clock::time_point LoopPacer::next(std::chrono::steady_clock::time_point now)
{
    _slot += _period;
    if (now > _slot)
    {
        ++_overruns;
        _slot += _period * ((now - _slot) / _period + 1);
    }
    return _slot;
}

}  // namespace vda5050_fleet_adapter_full_control::util
