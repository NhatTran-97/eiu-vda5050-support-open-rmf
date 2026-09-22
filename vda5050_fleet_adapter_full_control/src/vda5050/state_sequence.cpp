#include "vda5050_fleet_adapter_full_control/vda5050/state_sequence.hpp"

namespace vda5050_fleet_adapter_full_control::vda5050 {

bool StateSequence::is_stale(std::optional<std::uint32_t> header_id, std::optional<std::int64_t> timestamp_ms, int streak_limit)
{
    if (streak_limit <= 0 || !header_id)
    {
        return false;
    }

    const auto accept = [&]()
    {
        _header_id = header_id;
        _timestamp_ms = timestamp_ms;
        _stale_streak = 0;
        return false;
    };

    if (!_header_id || !timestamp_ms || !_timestamp_ms)
    {
        return accept();
    }
    if (*header_id > *_header_id || *timestamp_ms > *_timestamp_ms)
    {
        return accept();
    }
    if (++_stale_streak > streak_limit)
    {
        return accept();
    }
    return true;
}

void StateSequence::reset()
{
    _header_id.reset();
    _timestamp_ms.reset();
    _stale_streak = 0;
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
