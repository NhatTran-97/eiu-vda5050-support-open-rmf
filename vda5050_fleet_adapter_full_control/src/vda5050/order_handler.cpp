#include "vda5050_fleet_adapter_full_control/vda5050/order_handler.hpp"

#include <stdexcept>

#include "vda5050_fleet_adapter_full_control/vda5050/message_builder.hpp"

namespace vda5050_fleet_adapter_full_control::vda5050 {

nlohmann::json build_navigate_order(
    int header_id, const std::string &order_id,
    const std::string &manufacturer, const std::string &serial,
    const std::string &base_node_id, const RobotPose &base,
    const std::string &dest_node_id, const RobotPose &dest,
    const std::string &map_id,
    std::optional<double> speed_limit)
{
    if (order_id.empty())
    {
        throw std::invalid_argument(
            "build_navigate_order: order_id must not be empty -- the caller "
            "must generate it before calling, so it can track the same id "
            "make_order() ends up sending.");
    }

    nlohmann::json nodes = nlohmann::json::array();
    nodes.push_back(make_node(base_node_id, 0, base.x, base.y, base.theta, map_id));
    nodes.push_back(make_node(dest_node_id, 2, dest.x, dest.y, dest.theta, map_id));

    nlohmann::json edges = nlohmann::json::array();
    edges.push_back(make_edge("e_" + base_node_id + "_" + dest_node_id, 1,
                              base_node_id, dest_node_id, true, speed_limit));

    return make_order(header_id, manufacturer, serial, nodes, edges, order_id, 0);
}

}  // namespace vda5050_fleet_adapter_full_control::vda5050
