// The fleet adapter against a third-party VDA5050 2.1 AGV: vda-5050-lib's VirtualAgvAdapter (test/third_party).
// Needs VDA5050_TEST_BROKER and VDA5050_THIRD_PARTY_AGV=<manufacturer>/<serial> of a running virtual_agv.js.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <rclcpp/clock.hpp>
#include <rclcpp/logger.hpp>
#include <rmf_traffic/agv/Graph.hpp>
#include <rmf_traffic/agv/Planner.hpp>
#include <rmf_traffic/agv/VehicleTraits.hpp>
#include <rmf_traffic/geometry/Circle.hpp>

#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/robot_command_handle.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/message_builder.hpp"

namespace {

using namespace std::chrono_literals;
using vda5050_fleet_adapter_full_control::rmf::CommandStatus;
using vda5050_fleet_adapter_full_control::rmf::Connector;
using vda5050_fleet_adapter_full_control::rmf::RoutePolicy;
using vda5050_fleet_adapter_full_control::rmf::Transform;
using vda5050_fleet_adapter_full_control::rmf::VdaRobotCommandHandle;

const char *kInterface = "AMR";
const char *kMap = "tb3_world";
const char *kRobot = "tp";

// A(0,0) - B(2,0) - C(4,0), with D(2,2) off B; every lane runs both ways.
std::shared_ptr<rmf_traffic::agv::Graph> make_graph()
{
    auto graph = std::make_shared<rmf_traffic::agv::Graph>();
    const std::vector<std::pair<std::string, Eigen::Vector2d>> points = {
        {"A", {0.0, 0.0}}, {"B", {2.0, 0.0}}, {"C", {4.0, 0.0}}, {"D", {2.0, 2.0}}};
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        graph->add_waypoint(kMap, points[i].second);
        graph->add_key(points[i].first, i);
    }
    for (const auto &[a, b] : std::vector<std::pair<std::size_t, std::size_t>>{{0, 1}, {1, 2}, {1, 3}})
    {
        graph->add_lane(a, b);
        graph->add_lane(b, a);
    }
    return graph;
}

std::vector<rmf_traffic::agv::Plan::Waypoint> plan(const rmf_traffic::agv::Graph &graph, std::size_t start, std::size_t goal)
{
    const rmf_traffic::Profile profile{rmf_traffic::geometry::make_final_convex<rmf_traffic::geometry::Circle>(0.2)};
    const rmf_traffic::agv::VehicleTraits traits{{0.5, 0.7}, {0.6, 1.5}, profile};
    rmf_traffic::agv::Planner planner{rmf_traffic::agv::Planner::Configuration{graph, traits},
                                      rmf_traffic::agv::Planner::Options{nullptr}};
    const auto result = planner.plan(rmf_traffic::agv::Planner::Start{std::chrono::steady_clock::now(), start, 0.0},
                                     rmf_traffic::agv::Planner::Goal{goal});
    return result.success() ? result->get_waypoints() : std::vector<rmf_traffic::agv::Plan::Waypoint>{};
}

class ThirdPartyAgvTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const char *url = std::getenv("VDA5050_TEST_BROKER");
        const char *agv = std::getenv("VDA5050_THIRD_PARTY_AGV");
        const std::string identity = agv ? agv : "";
        const auto slash = identity.find('/');
        if (!url || slash == std::string::npos)
        {
            GTEST_SKIP() << "set VDA5050_TEST_BROKER and VDA5050_THIRD_PARTY_AGV=<manufacturer>/<serial> of a running virtual_agv.js";
        }
        graph = make_graph();
        connector = std::make_unique<Connector>(rclcpp::get_logger("third_party"), url, kInterface);
        connector->add_robot(kRobot, identity.substr(0, slash), identity.substr(slash + 1), Transform());
        connector->start();
        ASSERT_TRUE(pump([&] { return connector->metrics()["mqtt"]["connected"].get<bool>(); }, 5s));
        connector->request_state(kRobot);
        ASSERT_TRUE(pump([&] { return connector->get_data(kRobot).has_value(); }, 5s)) << "no state from the AGV";

        // Start every test idle at A.
        if (connector->order_in_progress(kRobot))
        {
            connector->stop(kRobot);
            pump([&] { return !connector->cancel_pending(kRobot); }, 5s);
        }
        const auto init = connector->init_position(kRobot, 0.0, 0.0, 0.0, kMap, "A");
        ASSERT_FALSE(init.empty());
        ASSERT_TRUE(pump([&] { return finished(init); }, 5s)) << "initPosition not finished";
    }

    void TearDown() override
    {
        handle.reset();
        if (connector)
        {
            connector->shutdown();
        }
    }

    std::shared_ptr<VdaRobotCommandHandle> make_handle(bool charge_at_chargers = false)
    {
        auto made = std::make_shared<VdaRobotCommandHandle>(rclcpp::get_logger("third_party"), kRobot, *connector, graph, 0.5,
                                                            std::make_shared<rclcpp::Clock>(RCL_STEADY_TIME), false, true, RoutePolicy{});
        vda5050_fleet_adapter_full_control::rmf::ActionPolicy policy;
        policy.charge_at_chargers = charge_at_chargers;
        made->set_action_policy(policy);
        return made;
    }

    // Start charging where the AGV stands and wait until it reports charging.
    void start_charging()
    {
        const auto start = connector->execute_instant_action(kRobot, "startCharging");
        ASSERT_FALSE(start.empty());
        ASSERT_TRUE(pump([&] { return finished(start) && connector->get_data(kRobot)->charging; }, 5s));
    }

    // Run the adapter's periodic work, as its update loop does, until `done` or `timeout`.
    bool pump(const std::function<bool()> &done, std::chrono::milliseconds timeout)
    {
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < until)
        {
            if (connector->metrics()["mqtt"]["connected"].get<bool>())
            {
                connector->poll(kRobot);
            }
            if (handle)
            {
                handle->expire_traffic_hold();
                handle->send_pending_order();
            }
            if (done())
            {
                return true;
            }
            std::this_thread::sleep_for(50ms);
        }
        return done();
    }

    bool finished(const std::string &action_id)
    {
        const auto result = connector->get_action_result(kRobot, action_id);
        return result.has_value() && result->first == "FINISHED";
    }

    bool at(double x, double y)
    {
        const auto data = connector->get_data(kRobot);
        return data.has_value() && std::hypot(data->position[0] - x, data->position[1] - y) < 0.1;
    }

    std::shared_ptr<rmf_traffic::agv::Graph> graph;
    std::unique_ptr<Connector> connector;
    std::shared_ptr<VdaRobotCommandHandle> handle;
};

const auto kNoEstimate = [](std::size_t, rmf_traffic::Duration) {};

}  // namespace

TEST_F(ThirdPartyAgvTest, ReportsAStateOnTheConfiguredMap)
{
    const auto data = connector->get_data(kRobot);
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(data->map_name, kMap);
    EXPECT_TRUE(connector->is_online(kRobot));
    EXPECT_TRUE(at(0.0, 0.0));
}

TEST_F(ThirdPartyAgvTest, DrivesARoutePlannedByRmfToItsGoal)
{
    handle = make_handle();
    bool done = false;
    handle->follow_new_path(plan(*graph, 0, 2), kNoEstimate, [&done]() { done = true; });
    ASSERT_TRUE(pump([&] { return connector->is_command_completed(kRobot); }, 10s));
    EXPECT_TRUE(at(4.0, 0.0));
    EXPECT_EQ(connector->get_data(kRobot)->last_node_id, "C");
}

TEST_F(ThirdPartyAgvTest, AcceptsHorizonReleasesAsOrderUpdates)
{
    const std::vector<Connector::RoutePoint> route = {{"B", 2.0, 0.0, 0.0, std::nullopt, ""}, {"C", 4.0, 0.0, 0.0, std::nullopt, ""}};
    const auto sent = connector->navigate_route(kRobot, route, kMap, 1);
    ASSERT_EQ(sent.status, CommandStatus::queued);
    ASSERT_TRUE(pump([&] { return at(2.0, 0.0); }, 10s)) << "the AGV did not drive the released base";
    std::this_thread::sleep_for(500ms);
    EXPECT_TRUE(at(2.0, 0.0)) << "the AGV drove past the released base";
    ASSERT_EQ(connector->release_more(kRobot, sent.order_id, 2), CommandStatus::queued);
    ASSERT_TRUE(pump([&] { return connector->is_command_completed(kRobot); }, 10s));
    EXPECT_TRUE(at(4.0, 0.0));
    EXPECT_FALSE(connector->refusal_of(kRobot, sent.order_id).has_value());
}

TEST_F(ThirdPartyAgvTest, PausesAndResumesAnOrder)
{
    const std::vector<Connector::RoutePoint> route = {{"B", 2.0, 0.0, 0.0, std::nullopt, ""}, {"C", 4.0, 0.0, 0.0, std::nullopt, ""}};
    ASSERT_EQ(connector->navigate_route(kRobot, route, kMap).status, CommandStatus::queued);
    ASSERT_EQ(connector->pause(kRobot), CommandStatus::queued);
    ASSERT_TRUE(pump([&] { return connector->get_data(kRobot)->paused; }, 5s));
    std::this_thread::sleep_for(500ms);
    EXPECT_FALSE(connector->is_command_completed(kRobot));
    ASSERT_EQ(connector->resume(kRobot), CommandStatus::queued);
    ASSERT_TRUE(pump([&] { return connector->is_command_completed(kRobot); }, 10s));
    EXPECT_FALSE(connector->get_data(kRobot)->paused);
}

TEST_F(ThirdPartyAgvTest, ANewPathWaitsForTheAgvToFinishTheCancel)
{
    handle = make_handle();
    handle->follow_new_path(plan(*graph, 0, 2), kNoEstimate, []() {});
    ASSERT_TRUE(pump([&] { return connector->get_data(kRobot)->velocity.has_value() || !at(0.0, 0.0); }, 5s));
    handle->follow_new_path(plan(*graph, 0, 3), kNoEstimate, []() {});
    EXPECT_TRUE(connector->cancel_pending(kRobot)) << "the replacement must wait for the cancelOrder";
    ASSERT_TRUE(pump([&] { return !connector->cancel_pending(kRobot) && connector->order_in_progress(kRobot); }, 10s));
    ASSERT_TRUE(pump([&] { return connector->is_command_completed(kRobot); }, 15s));
    EXPECT_TRUE(at(2.0, 2.0));
    EXPECT_EQ(connector->get_data(kRobot)->last_node_id, "D");
}

TEST_F(ThirdPartyAgvTest, ChargingActionsFinish)
{
    const auto start = connector->execute_instant_action(kRobot, "startCharging");
    ASSERT_FALSE(start.empty());
    ASSERT_TRUE(pump([&] { return finished(start) && connector->get_data(kRobot)->charging; }, 5s));
    const auto stop = connector->execute_instant_action(kRobot, "stopCharging");
    ASSERT_FALSE(stop.empty());
    ASSERT_TRUE(pump([&] { return finished(stop) && !connector->get_data(kRobot)->charging; }, 5s));
}

TEST_F(ThirdPartyAgvTest, ANewOrderWhileAnotherRunsIsRefusedAndReported)
{
    const std::vector<Connector::RoutePoint> to_c = {{"B", 2.0, 0.0, 0.0, std::nullopt, ""}, {"C", 4.0, 0.0, 0.0, std::nullopt, ""}};
    const auto first = connector->navigate_route(kRobot, to_c, kMap);
    ASSERT_EQ(first.status, CommandStatus::queued);
    ASSERT_TRUE(pump([&] { return connector->get_data(kRobot)->order_id == first.order_id; }, 5s));

    // Sent without cancelling the running order first.
    const std::vector<Connector::RoutePoint> to_d = {{"B", 2.0, 0.0, 0.0, std::nullopt, ""}, {"D", 2.0, 2.0, 0.0, std::nullopt, ""}};
    const auto second = connector->navigate_route(kRobot, to_d, kMap);
    ASSERT_EQ(second.status, CommandStatus::queued);
    ASSERT_TRUE(pump([&] { return connector->refusal_of(kRobot, second.order_id).has_value(); }, 5s));
    EXPECT_EQ(connector->get_data(kRobot)->order_id, first.order_id) << "the AGV keeps the running order";
}

TEST_F(ThirdPartyAgvTest, NodeActionsRunAtTheirNodesBeforeTheOrderCompletes)
{
    namespace vda = vda5050_fleet_adapter_full_control::vda5050;
    const nlohmann::json load = {{"stationType", "floor"}, {"loadType", "EPAL"}, {"duration", 1.0}};
    std::vector<Connector::RoutePoint> route = {{"B", 2.0, 0.0, 0.0, std::nullopt, ""}, {"C", 4.0, 0.0, 0.0, std::nullopt, ""}};
    route[0].actions.push_back(vda::make_action("pick", "HARD", "", load));
    route[1].actions.push_back(vda::make_action("drop", "HARD", "", load));
    const std::string pick = route[0].actions[0]["actionId"];
    const std::string drop = route[1].actions[0]["actionId"];
    ASSERT_EQ(connector->navigate_route(kRobot, route, kMap).status, CommandStatus::queued);
    ASSERT_TRUE(pump([&] { return at(4.0, 0.0); }, 15s));
    ASSERT_TRUE(pump([&] { return connector->is_command_completed(kRobot); }, 15s));
    EXPECT_EQ(connector->get_action_state(kRobot, pick), std::optional<std::string>("FINISHED"));
    EXPECT_EQ(connector->get_action_state(kRobot, drop), std::optional<std::string>("FINISHED"));
}

TEST_F(ThirdPartyAgvTest, AnOrderAfterStopChargingWaitsUntilTheAgvStoppedCharging)
{
    start_charging();
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 0, 2), kNoEstimate, []() {});
    ASSERT_TRUE(pump([&] { return connector->order_in_progress(kRobot); }, 5s));
    ASSERT_TRUE(pump([&] { return connector->is_command_completed(kRobot); }, 15s));
    EXPECT_FALSE(connector->get_data(kRobot)->charging);
    EXPECT_TRUE(at(4.0, 0.0));
    EXPECT_FALSE(connector->refusal_of(kRobot, connector->get_data(kRobot)->order_id).has_value());
}

TEST_F(ThirdPartyAgvTest, AnOrderRefusedWhileChargingIsNotCancelledOrPausedLater)
{
    start_charging();
    const std::vector<Connector::RoutePoint> route = {{"B", 2.0, 0.0, 0.0, std::nullopt, ""}};
    const auto refused = connector->navigate_route(kRobot, route, kMap);
    ASSERT_EQ(refused.status, CommandStatus::queued);
    ASSERT_TRUE(pump([&] { return connector->refusal_of(kRobot, refused.order_id).has_value(); }, 5s));
    EXPECT_FALSE(connector->order_in_progress(kRobot));

    handle = make_handle(true);
    handle->stop();
    handle->follow_new_path(plan(*graph, 0, 1), kNoEstimate, []() {});
    EXPECT_FALSE(connector->cancel_pending(kRobot)) << "no cancelOrder for an order the AGV never took";
    ASSERT_TRUE(pump([&] { return connector->is_command_completed(kRobot); }, 15s));
    EXPECT_TRUE(at(2.0, 0.0));
    EXPECT_FALSE(connector->get_data(kRobot)->paused);
}

TEST_F(ThirdPartyAgvTest, ATrafficHoldEndedAtOnceLeavesTheAgvDriving)
{
    handle = make_handle();
    const auto to_c = plan(*graph, 0, 2);
    handle->follow_new_path(to_c, kNoEstimate, []() {});
    ASSERT_TRUE(pump([&] { return !at(0.0, 0.0) && !at(4.0, 0.0); }, 5s));
    handle->stop();
    handle->follow_new_path(to_c, kNoEstimate, []() {});
    ASSERT_TRUE(pump([&] { return connector->is_command_completed(kRobot); }, 15s)) << "the AGV stayed paused";
    EXPECT_FALSE(connector->get_data(kRobot)->paused);
    EXPECT_TRUE(at(4.0, 0.0));
}
