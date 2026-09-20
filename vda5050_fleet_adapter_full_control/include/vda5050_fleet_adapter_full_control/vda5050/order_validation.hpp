#ifndef ORDER_VALIDATION_HPP
#define ORDER_VALIDATION_HPP

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "vda5050_fleet_adapter_full_control/vda5050/factsheet_handler.hpp"

namespace vda5050_fleet_adapter_full_control::vda5050 {

// A hard violation stops the message from being sent; a soft one is only reported.
enum class Severity
{
    hard,
    soft,
};

struct Violation
{
    Severity severity;
    std::string message;
};

// Facts about an order that is about to be published.
struct OrderShape
{
    std::size_t node_count = 0;
    std::size_t edge_count = 0;
    std::string map_id;
    // Every pose in the order, in the robot frame.
    std::vector<std::array<double, 3>> poses;
    std::optional<double> seconds_since_last_order;
};

// Check an order against the AGV's factsheet and the maps it reports.
std::vector<Violation> check_order(const OrderShape &order,
                                   const std::optional<ParsedFactsheet> &factsheet,
                                   const std::vector<std::string> &known_maps);

// Actions every AGV must accept; the factsheet never blocks them.
bool is_core_action(const std::string &action_type);

// Check that the AGV declared an instant action in its factsheet.
std::optional<Violation> check_instant_action(const std::string &action_type,  const std::optional<ParsedFactsheet> &factsheet);

bool has_hard_violation(const std::vector<Violation> &violations);

}  // namespace vda5050_fleet_adapter_full_control::vda5050

#endif  // ORDER_VALIDATION_HPP
