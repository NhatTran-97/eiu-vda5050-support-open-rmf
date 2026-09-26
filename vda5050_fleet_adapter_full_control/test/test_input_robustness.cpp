#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <rclcpp/logger.hpp>

#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/json_read.hpp"

using vda5050_fleet_adapter_full_control::rmf::Connector;
using vda5050_fleet_adapter_full_control::rmf::Transform;

namespace {

nlohmann::json state()
{
    using nlohmann::json;
    return {{"headerId", 1}, {"timestamp", "2026-09-23T10:00:00.000Z"}, {"orderId", ""}, {"lastNodeId", "n0"},
            {"driving", false}, {"nodeStates", json::array()}, {"edgeStates", json::array()},
            {"actionStates", json::array()}, {"errors", json::array()}, {"operatingMode", "AUTOMATIC"},
            {"safetyState", {{"eStop", "NONE"}, {"fieldViolation", false}}},
            {"batteryState", {{"batteryCharge", 80.0}}},
            {"agvPosition", {{"x", 1.0}, {"y", 2.0}, {"theta", 0.0}, {"mapId", "map"}, {"positionInitialized", true}}}};
}

nlohmann::json factsheet_with(const std::string &action)
{
    return {{"typeSpecification", {{"seriesName", "S"}}},
            {"protocolFeatures",
             {{"agvActions", nlohmann::json::array({{{"actionType", action}, {"blockingTypes", {"HARD"}}}})}}}};
}

// A connector that is never started: messages are fed in directly.
class InputRobustnessTest : public ::testing::Test
{
protected:
    InputRobustnessTest() : connector(rclcpp::get_logger("test"), "tcp://localhost:1", "AMR")
    {
        connector.add_robot("r1", "M", "S1", Transform());
    }

    void feed(const nlohmann::json &payload, const std::string &leaf = "state")
    {
        connector.handle_message("AMR/v2/M/S1/" + leaf, payload.dump());
    }

    std::uint64_t dropped(const char *kind) { return connector.metrics()["dropped"][kind].get<std::uint64_t>(); }

    Connector connector;
};

}  // namespace

TEST_F(InputRobustnessTest, AWellFormedStateIsAccepted)
{
    feed(state());
    EXPECT_TRUE(connector.is_online("r1"));
    ASSERT_TRUE(connector.get_data("r1").has_value());
}

TEST_F(InputRobustnessTest, AStateMissingARequiredFieldIsCountedAndDropped)
{
    auto s = state();
    s.erase("orderId");
    feed(s);
    EXPECT_FALSE(connector.is_online("r1"));
    EXPECT_EQ(dropped("invalid_state"), 1u);
}

TEST_F(InputRobustnessTest, InvalidJsonIsCountedAsABadPayload)
{
    connector.handle_message("AMR/v2/M/S1/state", "{not json");
    EXPECT_EQ(dropped("bad_payload"), 1u);
}

// Every optional field of the right name but the wrong JSON type must be handled inside the connector.
TEST_F(InputRobustnessTest, WrongFieldTypesInAStateNeverEscapeHandleMessage)
{
    const std::vector<std::pair<std::string, std::function<void(nlohmann::json &)>>> mutations = {
        {"agvPosition.x as string", [](nlohmann::json &s) { s["agvPosition"]["x"] = "1.0"; }},
        {"agvPosition.mapId as number", [](nlohmann::json &s) { s["agvPosition"]["mapId"] = 7; }},
        {"agvPosition.positionInitialized as string", [](nlohmann::json &s) { s["agvPosition"]["positionInitialized"] = "yes"; }},
        {"zoneSetId as number", [](nlohmann::json &s) { s["zoneSetId"] = 5; }},
        {"paused as number", [](nlohmann::json &s) { s["paused"] = 1; }},
        {"newBaseRequest as string", [](nlohmann::json &s) { s["newBaseRequest"] = "true"; }},
        {"batteryState.charging as string", [](nlohmann::json &s) { s["batteryState"]["charging"] = "no"; }},
        {"velocity.vx as string", [](nlohmann::json &s) { s["velocity"] = {{"vx", "fast"}}; }},
        {"lastNodeSequenceId as string", [](nlohmann::json &s) { s["lastNodeSequenceId"] = "3"; }},
        {"distanceSinceLastNode as string", [](nlohmann::json &s) { s["distanceSinceLastNode"] = "1m"; }},
        {"loads with a number", [](nlohmann::json &s) { s["loads"] = {1}; }},
        {"maps with a string", [](nlohmann::json &s) { s["maps"] = {"map"}; }},
    };
    for (const auto &[label, mutate] : mutations)
    {
        SCOPED_TRACE(label);
        auto s = state();
        mutate(s);
        EXPECT_NO_THROW(feed(s));
    }
}

TEST_F(InputRobustnessTest, WrongTypesInConnectionMessagesNeverEscapeHandleMessage)
{
    EXPECT_NO_THROW(feed({{"connectionState", 1}}, "connection"));
    // The same message from a robot of no fleet goes through discovery.
    EXPECT_NO_THROW(connector.handle_message("AMR/v2/X/Y/connection", R"({"connectionState": 1})"));
    EXPECT_NO_THROW(connector.handle_message(
        "AMR/v2/X/Y/state", R"({"agvPosition": {"x": 1, "y": 2, "mapId": 3, "positionInitialized": true}})"));
}

// A cached state whose actionStates hold non-objects must not make later queries throw on other threads.
TEST_F(InputRobustnessTest, NonObjectActionStatesDoNotBreakLaterQueries)
{
    feed(factsheet_with("dock"), "factsheet");
    auto s = state();
    s["actionStates"] = {1, "x"};
    ASSERT_NO_THROW(feed(s));
    ASSERT_TRUE(connector.is_online("r1"));

    EXPECT_NO_THROW(connector.get_action_state("r1", "a1"));
    EXPECT_NO_THROW(connector.get_action_result("r1", "a1"));
    EXPECT_NO_THROW(connector.is_command_completed("r1"));
    // RMF's dock() and PerformAction reach this from RMF's worker thread.
    EXPECT_NO_THROW(connector.execute_instant_action("r1", "dock"));
}

TEST_F(InputRobustnessTest, OutOfRangeLastNodeSequenceIdIsNotTakenAsProgress)
{
    for (const nlohmann::json &value : {nlohmann::json(-1), nlohmann::json(1e12)})
    {
        SCOPED_TRACE(value.dump());
        auto s = state();
        s["headerId"] = s["headerId"].get<int>() + 1;
        s["lastNodeSequenceId"] = value;
        feed({{"connectionState", "ONLINE"}}, "connection");
        feed(s);
        const auto data = connector.get_data("r1");
        ASSERT_TRUE(data.has_value());
        EXPECT_FALSE(data->last_node_sequence_id.has_value())
            << "reported as " << data->last_node_sequence_id.value_or(0);
    }
}

TEST_F(InputRobustnessTest, BatteryChargeOutsideZeroToHundredIsClamped)
{
    auto s = state();
    s["batteryState"]["batteryCharge"] = 4444;
    feed(s);
    ASSERT_TRUE(connector.get_data("r1").has_value());
    EXPECT_DOUBLE_EQ(connector.get_data("r1")->battery_soc, 1.0);
}

TEST_F(InputRobustnessTest, WarningAndFatalErrorsReachTheRobotData)
{
    auto s = state();
    s["errors"] = nlohmann::json::array({{{"errorType", "lowBattery"}, {"errorLevel", "WARNING"}, {"errorDescription", "12 %"}},
                                         {{"errorType", "motorFault"}, {"errorLevel", "FATAL"}},
                                         {{"errorType", 5}, {"errorLevel", "WARNING"}},
                                         {{"errorType", "unknownLevel"}, {"errorLevel", "INFO"}},
                                         "not an object"});
    feed(s);
    ASSERT_TRUE(connector.get_data("r1").has_value());
    const auto errors = connector.get_data("r1")->errors;
    ASSERT_EQ(errors.size(), 3u);
    EXPECT_EQ(errors[0].type, "lowBattery");
    EXPECT_EQ(errors[0].level, "WARNING");
    EXPECT_EQ(errors[0].description, "12 %");
    EXPECT_EQ(errors[1].type, "motorFault");
    EXPECT_EQ(errors[1].level, "FATAL");
    EXPECT_EQ(errors[2].type, "(unnamed error)");
    EXPECT_EQ(connector.get_data("r1")->fatal_error, "motorFault");
}

TEST(JsonRead, AFieldOfAnotherTypeReadsAsAbsent)
{
    namespace vda = vda5050_fleet_adapter_full_control::vda5050;
    const auto j = nlohmann::json::parse(R"({"s": "text", "n": 1.5, "b": true, "i": 7, "neg": -1, "big": 5000000000,
                                             "whole": 3.0, "arr": [1, {"id": 2, "name": "a"}, {"id": "x"}]})");
    EXPECT_EQ(vda::read_string(j, "s"), "text");
    EXPECT_FALSE(vda::read_string(j, "n").has_value());
    EXPECT_EQ(vda::read_number(j, "n"), 1.5);
    EXPECT_FALSE(vda::read_number(j, "s").has_value());
    EXPECT_EQ(vda::read_bool(j, "b"), true);
    EXPECT_FALSE(vda::read_bool(j, "i").has_value());
    EXPECT_EQ(vda::read_uint32(j, "i"), 7u);
    EXPECT_EQ(vda::read_uint32(j, "whole"), 3u);
    EXPECT_FALSE(vda::read_uint32(j, "neg").has_value());
    EXPECT_FALSE(vda::read_uint32(j, "big").has_value());
    EXPECT_FALSE(vda::read_uint32(j, "n").has_value());
    EXPECT_FALSE(vda::read_string(j, "missing").has_value());
    EXPECT_FALSE(vda::read_string(nlohmann::json::array(), "s").has_value());

    const auto objects = vda::read_objects(j, "arr", {"name", "id"});
    ASSERT_EQ(objects.size(), 2u);
    EXPECT_FALSE(objects[0].contains("id"));
    EXPECT_EQ(objects[0]["name"], "a");
    EXPECT_EQ(objects[1]["id"], "x");
    EXPECT_TRUE(vda::read_objects(j, "s").empty());
}
