#ifndef LINK_POLICY_HPP
#define LINK_POLICY_HPP

namespace vda5050_fleet_adapter_full_control::rmf {

// Time limits the connector applies to each AGV.
struct LinkPolicy
{
    // Time without a state message after which an AGV counts as offline.
    double state_timeout_s = 10.0;
    // Time an order may stay unacknowledged by the AGV before it counts as stuck.
    double order_stuck_timeout_s = 15.0;
    // Wait for the retained factsheet before the first request, and between further requests.
    double factsheet_first_wait_s = 5.0;
    double factsheet_retry_wait_s = 20.0;
    // Factsheet requests sent to one AGV before giving up; zero turns the requests off.
    int factsheet_request_attempts = 3;
};

}  // namespace vda5050_fleet_adapter_full_control::rmf

#endif  // LINK_POLICY_HPP
