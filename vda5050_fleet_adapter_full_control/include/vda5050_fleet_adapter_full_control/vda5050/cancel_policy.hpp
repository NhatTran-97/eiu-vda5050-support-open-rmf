#ifndef CANCEL_POLICY_HPP
#define CANCEL_POLICY_HPP

#include <chrono>

namespace vda5050_fleet_adapter_full_control::vda5050 {

// How long to wait for an AGV to answer a cancelOrder and how many times to send it.
struct CancelPolicy
{
    // Time after which an unanswered cancelOrder is judged; zero turns the tracking off.
    std::chrono::seconds confirm_timeout{5};
    // Times a cancelOrder may be sent for one order, counting the first.
    int attempts = 3;
};

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // CANCEL_POLICY_HPP
