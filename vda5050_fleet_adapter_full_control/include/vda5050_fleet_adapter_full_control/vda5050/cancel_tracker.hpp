#ifndef CANCEL_TRACKER_HPP
#define CANCEL_TRACKER_HPP

#include <chrono>
#include <string>

#include "vda5050_fleet_adapter_full_control/vda5050/cancel_policy.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/state_handler.hpp"

namespace vda5050_fleet_adapter_full_control::vda5050 {

// Follows a cancelOrder from the moment it is sent until the AGV has answered it.
class CancelTracker
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    enum class Outcome
    {
        none,           // nothing to report yet
        finished,       // the AGV finished the cancelOrder
        failed,         // the AGV reported the cancelOrder as FAILED
        order_gone,     // the AGV no longer reports the cancelled order as active
        still_running,  // the AGV acknowledged the cancelOrder but has not finished it
        resend,         // the AGV has not answered while it still reports the order: send it again
        unanswered      // the AGV has not answered after the last attempt
    };

    struct Verdict
    {
        Outcome outcome = Outcome::none;
        // The AGV's result description, its action status or the order it still reports.
        std::string detail;
    };

    // A cancelOrder with `action_id` for the tracked order `order_id` was sent at `now`.
    void sent(const std::string &action_id, const std::string &order_id, TimePoint now);

    // The pending cancelOrder was sent again under a new action id.
    void resent(const std::string &action_id, TimePoint now);

    void clear();
    bool pending() const { return _pending; }
    const std::string &order_id() const { return _order_id; }
    int attempts() const { return _attempts; }

    // Judge the pending cancelOrder against the newest state, received at `state_time`.
    // A verdict other than none, still_running and resend ends the tracking.
    Verdict assess(const ParsedState &state, TimePoint state_time, TimePoint now, const CancelPolicy &policy);

private:
    bool _pending = false;
    std::string _action_id;
    std::string _order_id;
    TimePoint _sent_at{};
    int _attempts = 0;
    bool _slow_reported = false;
};

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // CANCEL_TRACKER_HPP
