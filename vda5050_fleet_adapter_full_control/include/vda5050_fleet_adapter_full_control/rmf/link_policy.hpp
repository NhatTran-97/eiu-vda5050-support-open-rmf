#ifndef LINK_POLICY_HPP
#define LINK_POLICY_HPP

namespace vda5050_fleet_adapter_full_control::rmf {

// Time limits the connector applies to each AGV.
struct LinkPolicy
{
    // Time without a state message after which an AGV counts as offline.
    double state_timeout_s = 10.0;
    // State intervals an AGV declares in its factsheet that may pass without a state, when longer than state_timeout_s.
    double offline_state_intervals = 2.0;
    // Time an order may stay unacknowledged by the AGV before it counts as stuck.
    double order_stuck_timeout_s = 15.0;
    // Time for the AGV state to report a sent order or update before it is sent again unchanged.
    double order_ack_timeout_s = 5.0;
    // Times an unacknowledged order or update is sent again; zero turns resending off.
    int order_resend_attempts = 2;
    // Cancel an active order the AGV reports that this adapter did not send.
    bool cancel_unknown_orders = true;
    // Wait for the retained factsheet before the first request, and between further requests.
    double factsheet_first_wait_s = 5.0;
    double factsheet_retry_wait_s = 20.0;
    // Factsheet requests sent to one AGV before giving up; zero turns the requests off.
    int factsheet_request_attempts = 3;
};

}  // namespace vda5050_fleet_adapter_full_control::rmf

#endif  // LINK_POLICY_HPP
