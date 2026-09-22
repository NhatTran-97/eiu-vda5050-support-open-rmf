#include "vda5050_fleet_adapter_full_control/vda5050/cancel_tracker.hpp"

namespace vda5050_fleet_adapter_full_control::vda5050 {

void CancelTracker::sent(const std::string &action_id, const std::string &order_id, TimePoint now)
{
    _pending = true;
    _action_id = action_id;
    _order_id = order_id;
    _sent_at = now;
    _attempts = 1;
    _slow_reported = false;
}

void CancelTracker::resent(const std::string &action_id, TimePoint now)
{
    _action_id = action_id;
    _sent_at = now;
    ++_attempts;
    _slow_reported = false;
}

void CancelTracker::clear()
{
    _pending = false;
    _action_id.clear();
    _order_id.clear();
    _attempts = 0;
    _slow_reported = false;
}

CancelTracker::Verdict CancelTracker::assess(const ParsedState &state, TimePoint state_time, TimePoint now, const CancelPolicy &policy)
{
    if (!_pending || state_time < _sent_at)
    {
        return {};
    }
    const bool overdue = now - _sent_at >= policy.confirm_timeout;

    for (const auto &action : state.action_states)
    {
        if (action.value("actionId", std::string{}) != _action_id)
        {
            continue;
        }
        const std::string status = action.value("actionStatus", std::string{});
        const std::string description = action.value("resultDescription", std::string{});
        if (status == "FINISHED" || status == "FAILED")
        {
            clear();
            return {status == "FINISHED" ? Outcome::finished : Outcome::failed, description};
        }
        if (overdue && !_slow_reported)
        {
            _slow_reported = true;
            return {Outcome::still_running, status};
        }
        return {};
    }

    const bool order_active = state.order_id == _order_id && (!state.node_states.empty() || !state.edge_states.empty() || state.driving);
    if (!order_active)
    {
        clear();
        return {Outcome::order_gone, {}};
    }
    if (!overdue)
    {
        return {};
    }
    if (_attempts >= policy.attempts)
    {
        const std::string order = _order_id;
        clear();
        return {Outcome::unanswered, order};
    }
    return {Outcome::resend, _order_id};
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
