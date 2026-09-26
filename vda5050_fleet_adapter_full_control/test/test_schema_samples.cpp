// Writes every kind of message the fleet adapter publishes to VDA5050_SCHEMA_SAMPLES,
// for validate_schemas.py to check against the official VDA5050 2.1 JSON schemas.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "vda5050_fleet_adapter_full_control/vda5050/instant_action_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/message_builder.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/order_handler.hpp"

namespace vda = vda5050_fleet_adapter_full_control::vda5050;

namespace {

const char *kManufacturer = "ROBOTIS";
const char *kSerial = "0001";

std::filesystem::path samples_dir()
{
    const char *dir = std::getenv("VDA5050_SCHEMA_SAMPLES");
    return dir ? std::filesystem::path(dir) : std::filesystem::path();
}

void write_sample(const std::string &name, const nlohmann::json &message)
{
    std::ofstream(samples_dir() / (name + ".json")) << message.dump(2);
}

std::vector<vda::RouteWaypoint> route()
{
    return {{"B", {2.0, 0.0, 0.0}, 0.4, "floor_1"},
            {"C", {4.0, 0.0, 1.57}, std::nullopt, "floor_1"},
            {"C2", {14.0, 0.0, 1.57}, std::nullopt, "floor_2"}};
}

}  // namespace

TEST(SchemaSamples, FleetAdapterMessages)
{
    if (samples_dir().empty())
    {
        GTEST_SKIP() << "set VDA5050_SCHEMA_SAMPLES to a directory";
    }
    std::filesystem::create_directories(samples_dir());
    const vda::RobotPose base{0.0, 0.0, 0.0};
    const vda::NodeDeviation deviation{0.3, 0.5};

    write_sample("order__new_route_with_horizon",
                 vda::build_route_order(1, "order-1", kManufacturer, kSerial, "A", base, route(), "floor_1", 0, 2, 0, deviation));
    write_sample("order__update_from_released_node",
                 vda::build_route_order(2, "order-1", kManufacturer, kSerial, "A", base, route(), "floor_1", 1, 3, 2, deviation));
    write_sample("order__single_node", vda::build_route_order(3, "order-2", kManufacturer, kSerial, "A", base, {route().front()}, "floor_1"));
    auto with_action = route();
    with_action[1].actions.push_back(vda::make_action("pick", "HARD", "", {{"stationType", "floor"}, {"loadType", "EPAL"}}));
    write_sample("order__node_action", vda::build_route_order(4, "order-3", kManufacturer, kSerial, "A", base, with_action, "floor_1"));

    int header = 1;
    write_sample("instantActions__cancelOrder", vda::build_cancel_order(header++, kManufacturer, kSerial));
    write_sample("instantActions__cancelOrder_unknown_order",
                 vda::build_instant_action(header++, kManufacturer, kSerial, "cancelOrder", {{"orderId", "foreign"}}).message);
    write_sample("instantActions__startPause", vda::build_start_pause(header++, kManufacturer, kSerial));
    write_sample("instantActions__stopPause", vda::build_stop_pause(header++, kManufacturer, kSerial));
    write_sample("instantActions__stateRequest", vda::build_state_request(header++, kManufacturer, kSerial));
    write_sample("instantActions__factsheetRequest",
                 vda::build_instant_action(header++, kManufacturer, kSerial, "factsheetRequest", nlohmann::json::object(), "NONE").message);
    write_sample("instantActions__initPosition",
                 vda::build_instant_action(header++, kManufacturer, kSerial, "initPosition",
                                           {{"x", 1.5}, {"y", -2.0}, {"theta", 0.3}, {"mapId", "floor_1"}, {"lastNodeId", "B"}}, "NONE").message);
    write_sample("instantActions__startCharging", vda::build_instant_action(header++, kManufacturer, kSerial, "startCharging").message);
    write_sample("instantActions__stopCharging", vda::build_instant_action(header++, kManufacturer, kSerial, "stopCharging").message);
    write_sample("instantActions__dock_action_with_parameters",
                 vda::build_instant_action(header++, kManufacturer, kSerial, "finePositioning",
                                           {{"stationName", "station_1"}, {"height", 1.5}, {"count", 2}, {"exact", true}}).message);

    std::size_t written = 0;
    for (const auto &entry : std::filesystem::directory_iterator(samples_dir()))
    {
        written += entry.path().extension() == ".json";
    }
    EXPECT_GE(written, 14u);
}
