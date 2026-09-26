#include <gtest/gtest.h>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <mqtt/async_client.h>
#include <rclcpp/clock.hpp>
#include <rclcpp/init_options.hpp>
#include <rclcpp/logger.hpp>
#include <rclcpp/utilities.hpp>
#include <rmf_battery/agv/BatterySystem.hpp>
#include <rmf_battery/agv/MechanicalSystem.hpp>
#include <rmf_battery/agv/PowerSystem.hpp>
#include <rmf_battery/agv/SimpleDevicePowerSink.hpp>
#include <rmf_battery/agv/SimpleMotionPowerSink.hpp>
#include <rmf_fleet_adapter/agv/test/MockAdapter.hpp>
#include <rmf_traffic/agv/Graph.hpp>
#include <rmf_traffic/agv/Planner.hpp>
#include <rmf_traffic/agv/VehicleTraits.hpp>
#include <rmf_traffic/geometry/Circle.hpp>

#include "vda5050_fleet_adapter_full_control/mqtt/mqtt_client.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/robot_command_handle.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/message_builder.hpp"

extern char **environ;

namespace {

using namespace std::chrono_literals;
using vda5050_fleet_adapter_full_control::rmf::CommandStatus;
using vda5050_fleet_adapter_full_control::rmf::Connector;
using vda5050_fleet_adapter_full_control::rmf::LevelMaps;
using vda5050_fleet_adapter_full_control::rmf::RoutePolicy;
using vda5050_fleet_adapter_full_control::rmf::Transform;
using vda5050_fleet_adapter_full_control::rmf::VdaRobotCommandHandle;
using Waypoints = std::vector<rmf_traffic::agv::Plan::Waypoint>;

const char *kInterface = "RV";
const char *kMap = "L1";
// ROS domain of the RMF tests, kept to this host.
constexpr std::size_t kRosDomain = 77;

// Broker for these tests, e.g. tcp://127.0.0.1:18830; the tests skip without one.
std::optional<std::string> broker_url()
{
    const char *url = std::getenv("VDA5050_TEST_BROKER");
    return url && *url ? std::optional<std::string>(url) : std::nullopt;
}

// Records the order and instantActions messages the adapter publishes for one robot.
class Capture : public virtual mqtt::callback
{
public:
    explicit Capture(const std::string &url) : _client(url, "review_capture_" + std::to_string(::getpid())) { _client.set_callback(*this); }

    ~Capture() override
    {
        try
        {
            _client.disconnect()->wait_for(1s);
        }
        catch (...)
        {
        }
    }

    bool start()
    {
        mqtt::connect_options options;
        options.set_clean_session(true);
        if (!_client.connect(options)->wait_for(5s))
        {
            return false;
        }
        return _client.subscribe(std::string(kInterface) + "/v2/M/S1/+", 1)->wait_for(5s);
    }

    void message_arrived(mqtt::const_message_ptr msg) override
    {
        const std::string topic = msg->get_topic();
        const std::string leaf = topic.substr(topic.rfind('/') + 1);
        if (leaf != "order" && leaf != "instantActions")
        {
            return;
        }
        const auto payload = nlohmann::json::parse(msg->get_payload_str(), nullptr, false);
        const std::string kind = leaf == "order" ? "order" : payload["actions"][0].value("actionType", std::string{"?"});
        // Requests for a state or factsheet are housekeeping, not commands.
        if (kind == "stateRequest" || kind == "factsheetRequest")
        {
            return;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _messages.push_back(payload);
        _kinds.push_back(kind);
        _qos.push_back(msg->get_qos());
        _cv.notify_all();
    }

    // Waits until `count` messages arrived, or the timeout passed.
    bool wait_for(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _cv.wait_for(lock, timeout, [&] { return _kinds.size() >= count; });
    }

    std::vector<std::string> kinds()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _kinds;
    }

    nlohmann::json message(std::size_t index)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _messages.at(index);
    }

    int qos(std::size_t index)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _qos.at(index);
    }

private:
    mqtt::async_client _client;
    std::mutex _mutex;
    std::condition_variable _cv;
    std::vector<nlohmann::json> _messages;
    std::vector<std::string> _kinds;
    std::vector<int> _qos;
};

std::string join(const std::vector<std::string> &items)
{
    std::string out;
    for (const auto &item : items)
    {
        out += (out.empty() ? "" : ", ") + item;
    }
    return "[" + out + "]";
}

// A(0,0) - B(2,0) - C(4,0), with D(2,2) off B; lanes run both ways, A and C are chargers.
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
    graph->get_waypoint(0).set_charger(true);
    graph->get_waypoint(2).set_charger(true);
    for (const auto &[a, b] : std::vector<std::pair<std::size_t, std::size_t>>{{0, 1}, {1, 2}, {1, 3}})
    {
        graph->add_lane(a, b);
        graph->add_lane(b, a);
    }
    return graph;
}

rmf_traffic::agv::VehicleTraits traits()
{
    const rmf_traffic::Profile profile{rmf_traffic::geometry::make_final_convex<rmf_traffic::geometry::Circle>(0.2)};
    return {{0.5, 0.7}, {0.6, 1.5}, profile};
}

Waypoints plan(const rmf_traffic::agv::Graph &graph, std::size_t goal)
{
    rmf_traffic::agv::Planner planner{rmf_traffic::agv::Planner::Configuration{graph, traits()},
                                      rmf_traffic::agv::Planner::Options{nullptr}};
    const auto result = planner.plan(rmf_traffic::agv::Planner::Start{std::chrono::steady_clock::now(), 0, 0.0},
                                     rmf_traffic::agv::Planner::Goal{goal});
    return result.success() ? result->get_waypoints() : Waypoints{};
}

nlohmann::json agv_state(int header, const std::string &order_id, const std::string &last_node,
                         std::optional<int> last_sequence, const nlohmann::json &node_states, bool driving)
{
    nlohmann::json s = {{"headerId", header},
                        {"orderId", order_id},
                        {"lastNodeId", last_node},
                        {"driving", driving},
                        {"nodeStates", node_states},
                        {"edgeStates", nlohmann::json::array()},
                        {"actionStates", nlohmann::json::array()},
                        {"errors", nlohmann::json::array()},
                        {"operatingMode", "AUTOMATIC"},
                        {"safetyState", {{"eStop", "NONE"}, {"fieldViolation", false}}},
                        {"batteryState", {{"batteryCharge", 80.0}}},
                        {"maps", nlohmann::json::array({{{"mapId", kMap}, {"mapStatus", "ENABLED"}}})},
                        {"agvPosition", {{"x", 0.0}, {"y", 0.0}, {"theta", 0.0}, {"mapId", kMap}, {"positionInitialized", true}}}};
    if (last_sequence.has_value())
    {
        s["lastNodeSequenceId"] = *last_sequence;
    }
    return s;
}

class CommandHandleBrokerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto url = broker_url();
        if (!url)
        {
            GTEST_SKIP() << "set VDA5050_TEST_BROKER to a private broker to run these tests";
        }
        graph = make_graph();
        connector = std::make_unique<Connector>(rclcpp::get_logger("test"), *url, kInterface);
        register_robot();
        connector->start();
        capture = std::make_unique<Capture>(*url);
        ASSERT_TRUE(capture->start());
        for (int i = 0; i < 100 && !connector->metrics()["mqtt"]["connected"].get<bool>(); ++i)
        {
            std::this_thread::sleep_for(50ms);
        }
        ASSERT_TRUE(connector->metrics()["mqtt"]["connected"].get<bool>());
        std::this_thread::sleep_for(200ms);
        feed(agv_state(1, "", "", std::nullopt, nlohmann::json::array(), false));
    }

    virtual void register_robot()
    {
        connector->add_robot("r1", "M", "S1", Transform());
    }

    void TearDown() override
    {
        handle.reset();
        capture.reset();
        if (connector)
        {
            connector->shutdown();
        }
    }

    std::shared_ptr<VdaRobotCommandHandle> make_handle(bool stitch, double traffic_pause_timeout_s = 10.0, bool cap_speed = false)
    {
        RoutePolicy policy;
        policy.traffic_pause_timeout_s = traffic_pause_timeout_s;
        policy.cap_speed_to_fleet = cap_speed;
        return std::make_shared<VdaRobotCommandHandle>(rclcpp::get_logger("test"), "r1", *connector, graph, 0.5,
                                                       std::make_shared<rclcpp::Clock>(RCL_STEADY_TIME), false, stitch, policy);
    }

    void feed(const nlohmann::json &state)
    {
        connector->handle_message(std::string(kInterface) + "/v2/M/S1/state", state.dump());
    }

    // Report the order at `index` as accepted and driven from its first node.
    void acknowledge(std::size_t index, int header, bool paused = false)
    {
        const auto order = capture->message(index);
        nlohmann::json remaining = nlohmann::json::array();
        for (std::size_t i = 1; i < order["nodes"].size(); ++i)
        {
            remaining.push_back({{"nodeId", order["nodes"][i]["nodeId"]}, {"sequenceId", order["nodes"][i]["sequenceId"]}, {"released", true}});
        }
        auto state = agv_state(header, order["orderId"], order["nodes"][0]["nodeId"], 0, remaining, !paused);
        state["paused"] = paused;
        feed(state);
    }

    // Report the order at `index` as finished at its final node.
    void finish(std::size_t index, int header)
    {
        const auto order = capture->message(index);
        const auto &last = order["nodes"].back();
        feed(agv_state(header, order["orderId"], last["nodeId"], last["sequenceId"].get<int>(), nlohmann::json::array(), false));
    }

    // Report the cancelOrder at `index` as finished with no order left, and let the connector judge it.
    void answer_cancel(std::size_t index, int header, bool paused = false)
    {
        const auto cancel = capture->message(index)["actions"][0];
        auto state = agv_state(header, "", "", std::nullopt, nlohmann::json::array(), false);
        state["paused"] = paused;
        state["actionStates"] = nlohmann::json::array({{{"actionId", cancel["actionId"]}, {"actionType", "cancelOrder"}, {"actionStatus", "FINISHED"}}});
        feed(state);
        connector->poll("r1");
    }

    // B and C of the graph as connector route points.
    static std::vector<Connector::RoutePoint> route_to_c()
    {
        return {{"B", 2.0, 0.0, 0.0, std::nullopt, ""}, {"C", 4.0, 0.0, 0.0, std::nullopt, ""}};
    }

    std::shared_ptr<rmf_traffic::agv::Graph> graph;
    std::unique_ptr<Connector> connector;
    std::unique_ptr<Capture> capture;
    std::shared_ptr<VdaRobotCommandHandle> handle;
};

using Kinds = std::vector<std::string>;

const auto kNoEstimate = [](std::size_t, rmf_traffic::Duration) {};
const auto kNoCallback = []() {};

}  // namespace

TEST_F(CommandHandleBrokerTest, APathIsSentAsOneOrderToItsGoal)
{
    handle = make_handle(true);
    const auto to_c = plan(*graph, 2);
    ASSERT_FALSE(to_c.empty());
    handle->follow_new_path(to_c, kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    EXPECT_EQ(capture->kinds(), std::vector<std::string>{"order"});
    EXPECT_EQ(capture->message(0)["nodes"].back()["nodeId"], "C");
}

TEST_F(CommandHandleBrokerTest, TheSameRouteAfterATrafficHoldOnlyUnpauses)
{
    handle = make_handle(true);
    const auto to_c = plan(*graph, 2);
    handle->follow_new_path(to_c, kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->stop();
    handle->follow_new_path(to_c, kNoEstimate, kNoCallback);
    std::this_thread::sleep_for(300ms);
    handle->expire_traffic_hold();
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause"})) << "stopPause waits for the AGV to answer the startPause";
    acknowledge(0, 3, true);
    handle->expire_traffic_hold();
    capture->wait_for(3, 1s);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause", "stopPause"}));
}

TEST_F(CommandHandleBrokerTest, NoOrderGoesToAnAgvWithoutAPose)
{
    handle = make_handle(true);
    auto lost = agv_state(2, "", "", std::nullopt, nlohmann::json::array(), false);
    lost["agvPosition"]["positionInitialized"] = false;
    feed(lost);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    std::this_thread::sleep_for(500ms);
    EXPECT_TRUE(capture->kinds().empty()) << join(capture->kinds());
}

TEST_F(CommandHandleBrokerTest, AStopWithoutAnOrderSendsNothing)
{
    handle = make_handle(true, 0.2);
    handle->stop();
    std::this_thread::sleep_for(300ms);
    handle->expire_traffic_hold();
    std::this_thread::sleep_for(500ms);
    EXPECT_TRUE(capture->kinds().empty()) << join(capture->kinds());
}

TEST_F(CommandHandleBrokerTest, AStopAfterTheOrderFinishedSendsNothing)
{
    handle = make_handle(true, 0.2);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    finish(0, 2);
    handle->stop();
    std::this_thread::sleep_for(300ms);
    handle->expire_traffic_hold();
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), Kinds{"order"});
}

TEST_F(CommandHandleBrokerTest, RepeatedStopsSendOneStartPause)
{
    handle = make_handle(true);
    const auto to_c = plan(*graph, 2);
    handle->follow_new_path(to_c, kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->stop();
    handle->stop();
    ASSERT_TRUE(capture->wait_for(2, 3s));
    acknowledge(0, 3, true);
    handle->stop();
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause"})) << "the AGV is already paused or pausing";

    handle->follow_new_path(to_c, kNoEstimate, kNoCallback);
    handle->stop();
    capture->wait_for(4, 2s);
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause", "stopPause", "startPause"}))
        << "after a stopPause the stale paused state does not suppress the next startPause";
}

TEST_F(CommandHandleBrokerTest, AHeldOrderIsCancelledBeforeADifferentRouteReplacesIt)
{
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->stop();
    ASSERT_TRUE(capture->wait_for(2, 3s));
    acknowledge(0, 3, true);
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(3, 2s));
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause", "cancelOrder"}));
    answer_cancel(2, 4, true);
    handle->send_pending_order();
    capture->wait_for(5, 2s);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause", "cancelOrder", "order", "stopPause"}));
}

TEST_F(CommandHandleBrokerTest, NoStopPauseForAnAgvThatLeftThePauseWithTheCancel)
{
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->stop();
    ASSERT_TRUE(capture->wait_for(2, 3s));
    acknowledge(0, 3, true);
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(3, 2s));
    answer_cancel(2, 4, false);
    handle->send_pending_order();
    capture->wait_for(4, 2s);
    std::this_thread::sleep_for(300ms);
    handle->expire_traffic_hold();
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause", "cancelOrder", "order"}));
}

TEST_F(CommandHandleBrokerTest, ASupersededOrderIsCancelledBeforeItsReplacement)
{
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(2, 2s));
    answer_cancel(1, 3);
    handle->send_pending_order();
    capture->wait_for(3, 2s);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "cancelOrder", "order"}));
}

TEST_F(CommandHandleBrokerTest, ATrafficHoldEndsEvenWhenItsReplacementIsRefused)
{
    handle = make_handle(true, 0.3);
    connector->handle_message(std::string(kInterface) + "/v2/M/S1/factsheet",
                              R"({"typeSpecification": {"seriesName": "S"},
                                  "protocolLimits": {"maxArrayLens": {"order.nodes": 3, "order.edges": 2}}})");
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->stop();
    ASSERT_TRUE(capture->wait_for(2, 3s));
    acknowledge(0, 3, true);
    // Four nodes, over the AGV's limit of three.
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(3, 2s));
    answer_cancel(2, 4, true);
    handle->send_pending_order();
    std::this_thread::sleep_for(500ms);
    handle->expire_traffic_hold();
    std::this_thread::sleep_for(300ms);
    handle->expire_traffic_hold();
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause", "cancelOrder", "stopPause"}));
}

TEST_F(CommandHandleBrokerTest, ARefusedOrderIsNotTreatedAsRunningOnAnIdleAgv)
{
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    auto refused = agv_state(2, "", "", std::nullopt, nlohmann::json::array(), false);
    refused["errors"] = nlohmann::json::array({{{"errorType", "orderError"}, {"errorLevel", "WARNING"},
                                                {"errorReferences", {{{"referenceKey", "orderId"}, {"referenceValue", capture->message(0)["orderId"]}}}}}});
    feed(refused);
    EXPECT_FALSE(connector->order_in_progress("r1"));
    handle->stop();
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    capture->wait_for(2, 2s);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "order"})) << "no startPause or cancelOrder for an order the AGV never took";
}

TEST_F(CommandHandleBrokerTest, ANewPathWithoutAPoseCancelsTheOrderItReplaces)
{
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    auto lost = agv_state(3, capture->message(0)["orderId"], "", 0, nlohmann::json::array({{{"nodeId", "C"}}}), false);
    lost["agvPosition"]["positionInitialized"] = false;
    feed(lost);
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "cancelOrder"}));
}

TEST_F(CommandHandleBrokerTest, AnOrderRunsUntilTheAgvReportsItFinishedAtItsFinalNode)
{
    EXPECT_FALSE(connector->order_in_progress("r1"));
    ASSERT_EQ(connector->navigate_route("r1", route_to_c(), kMap).status, CommandStatus::queued);
    EXPECT_TRUE(connector->order_in_progress("r1"));
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    EXPECT_TRUE(connector->order_in_progress("r1"));

    // Empty node states short of the final node do not finish it.
    const auto order = capture->message(0);
    feed(agv_state(3, order["orderId"], "B", 2, nlohmann::json::array(), false));
    EXPECT_TRUE(connector->order_in_progress("r1"));

    finish(0, 4);
    EXPECT_FALSE(connector->order_in_progress("r1"));
    // An AGV that restarts and reports no order does not bring it back.
    feed(agv_state(5, "", "", std::nullopt, nlohmann::json::array(), false));
    EXPECT_FALSE(connector->order_in_progress("r1"));
}

TEST_F(CommandHandleBrokerTest, AHorizonReleaseOnlyGrowsTheTrackedOrder)
{
    const auto first = connector->navigate_route("r1", route_to_c(), kMap, 1);
    const auto second = connector->navigate_route("r1", route_to_c(), kMap, 1);
    ASSERT_EQ(second.status, CommandStatus::queued);
    ASSERT_TRUE(capture->wait_for(2, 3s));

    EXPECT_EQ(connector->release_more("r1", first.order_id, 2), CommandStatus::rejected);
    EXPECT_EQ(connector->release_more("r1", second.order_id, 2), CommandStatus::queued);
    EXPECT_EQ(connector->release_more("r1", second.order_id, 2), CommandStatus::rejected);
    capture->wait_for(3, 2s);
    std::this_thread::sleep_for(200ms);
    ASSERT_EQ(capture->kinds(), (Kinds{"order", "order", "order"}));
    EXPECT_EQ(capture->message(2)["orderId"], second.order_id);
    EXPECT_EQ(capture->message(2)["orderUpdateId"], 1);
}

TEST_F(CommandHandleBrokerTest, OrderNodesCarryTheConfiguredDeviation)
{
    connector->set_node_deviation({0.2, 0.35});
    ASSERT_EQ(connector->navigate_route("r1", route_to_c(), kMap).status, CommandStatus::queued);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    const auto order = capture->message(0);
    for (const auto &node : order["nodes"])
    {
        EXPECT_DOUBLE_EQ(node["nodePosition"]["allowedDeviationXY"].get<double>(), 0.2);
        EXPECT_DOUBLE_EQ(node["nodePosition"]["allowedDeviationTheta"].get<double>(), 0.35);
    }
}

TEST_F(CommandHandleBrokerTest, EdgeSpeedIsCappedAtTheFleetSpeedOnlyWhenConfigured)
{
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    const auto uncapped = capture->message(0);
    for (const auto &edge : uncapped["edges"])
    {
        EXPECT_FALSE(edge.contains("maxSpeed"));
    }

    handle = make_handle(true, 10.0, true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(2, 3s));
    answer_cancel(1, 2);
    handle->send_pending_order();
    ASSERT_TRUE(capture->wait_for(3, 3s));
    const auto capped = capture->message(capture->kinds().size() - 1);
    ASSERT_FALSE(capped["edges"].empty());
    for (const auto &edge : capped["edges"])
    {
        EXPECT_DOUBLE_EQ(edge["maxSpeed"].get<double>(), 0.5);
    }
}

TEST_F(CommandHandleBrokerTest, NodeActionsTravelWithTheirNodeAndHoldTheOrderUntilTheyEnd)
{
    auto route = route_to_c();
    route.back().actions.push_back(vda5050_fleet_adapter_full_control::vda5050::make_action("pick", "HARD", "pick-1", {{"stationType", "floor"}}));
    ASSERT_EQ(connector->navigate_route("r1", route, kMap).status, CommandStatus::queued);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    const auto order = capture->message(0);
    EXPECT_TRUE(order["nodes"][1]["actions"].empty());
    ASSERT_EQ(order["nodes"][2]["actions"].size(), 1u);
    EXPECT_EQ(order["nodes"][2]["actions"][0]["actionType"], "pick");

    auto picking = agv_state(2, order["orderId"], "C", 4, nlohmann::json::array(), false);
    picking["actionStates"] = nlohmann::json::array({{{"actionId", "pick-1"}, {"actionType", "pick"}, {"actionStatus", "RUNNING"}}});
    feed(picking);
    EXPECT_FALSE(connector->is_command_completed("r1")) << "the pick has not ended";
    auto picked = picking;
    picked["headerId"] = 3;
    picked["actionStates"][0]["actionStatus"] = "FINISHED";
    feed(picked);
    EXPECT_TRUE(connector->is_command_completed("r1"));
}

TEST_F(CommandHandleBrokerTest, OrdersUseTheConfiguredQos)
{
    ASSERT_EQ(connector->navigate_route("r1", route_to_c(), kMap).status, CommandStatus::queued);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    EXPECT_EQ(capture->qos(0), 1);

    vda5050_fleet_adapter_full_control::mqtt::MqttOptions options;
    options.qos = 0;
    Connector best_effort(rclcpp::get_logger("test"), *broker_url(), kInterface, std::nullopt, std::nullopt, options);
    best_effort.add_robot("r1", "M", "S1", Transform());
    best_effort.start();
    for (int i = 0; i < 100 && !best_effort.metrics()["mqtt"]["connected"].get<bool>(); ++i)
    {
        std::this_thread::sleep_for(50ms);
    }
    best_effort.handle_message(std::string(kInterface) + "/v2/M/S1/state", agv_state(1, "", "", std::nullopt, nlohmann::json::array(), false).dump());
    ASSERT_EQ(best_effort.navigate_route("r1", route_to_c(), kMap).status, CommandStatus::queued);
    ASSERT_TRUE(capture->wait_for(2, 3s));
    EXPECT_EQ(capture->qos(1), 0);
    best_effort.shutdown();
}

TEST_F(CommandHandleBrokerTest, InitPositionCarriesTheLastNodeId)
{
    ASSERT_FALSE(connector->init_position("r1", 2.0, 0.0, 0.0, kMap, "B").empty());
    ASSERT_TRUE(capture->wait_for(1, 3s));
    const auto message = capture->message(0);
    std::map<std::string, nlohmann::json> parameters;
    for (const auto &p : message["actions"][0]["actionParameters"])
    {
        parameters[p["key"].get<std::string>()] = p["value"];
    }
    EXPECT_EQ(message["actions"][0]["actionType"], "initPosition");
    EXPECT_EQ(parameters["lastNodeId"], "B");
    EXPECT_EQ(parameters["mapId"], kMap);
}

TEST_F(CommandHandleBrokerTest, DocksAreSentAsTheirConfiguredActions)
{
    handle = make_handle(true);
    vda5050_fleet_adapter_full_control::rmf::ActionPolicy policy;
    policy.dock_actions["parking"] = {"finePositioning", nlohmann::json::object()};
    policy.dock_actions["station_dock"] = {"finePositioning", {{"stationName", "station_1"}}};
    handle->set_action_policy(policy);
    handle->dock("parking", kNoCallback);
    handle->dock("station_dock", kNoCallback);
    handle->dock("other_dock", kNoCallback);
    ASSERT_TRUE(capture->wait_for(3, 3s));
    EXPECT_EQ(capture->kinds(), (Kinds{"finePositioning", "finePositioning", "other_dock"}));
    const auto fine = capture->message(1)["actions"][0];
    ASSERT_TRUE(fine.contains("actionParameters"));
    EXPECT_EQ(fine["actionParameters"][0]["key"], "stationName");
    EXPECT_EQ(fine["actionParameters"][0]["value"], "station_1");
}

TEST_F(CommandHandleBrokerTest, ANewOrderWaitsForTheCancelToBeConfirmed)
{
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    std::this_thread::sleep_for(300ms);
    handle->send_pending_order();
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "cancelOrder"}));

    handle->follow_new_path(plan(*graph, 1), kNoEstimate, kNoCallback);
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "cancelOrder"})) << "a newer path replaces the waiting order without a second cancelOrder";

    answer_cancel(1, 3);
    handle->send_pending_order();
    ASSERT_TRUE(capture->wait_for(3, 2s));
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "cancelOrder", "order"}));
    EXPECT_EQ(capture->message(2)["nodes"].back()["nodeId"], "B");
}

TEST_F(CommandHandleBrokerTest, AStopDropsTheOrderWaitingForTheCancel)
{
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(2, 2s));
    handle->stop();
    answer_cancel(1, 3);
    handle->send_pending_order();
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "cancelOrder"}));
}

TEST_F(CommandHandleBrokerTest, AnUnansweredCancelStillLetsTheNewOrderGo)
{
    connector->set_cancel_policy({std::chrono::seconds(1), 1});
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(2, 2s));
    std::this_thread::sleep_for(1200ms);
    acknowledge(0, 3);
    connector->poll("r1");
    handle->send_pending_order();
    ASSERT_TRUE(capture->wait_for(3, 2s));
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "cancelOrder", "order"}));
}

TEST_F(CommandHandleBrokerTest, WithoutCancelTrackingTheNewOrderFollowsAtOnce)
{
    connector->set_cancel_policy({std::chrono::seconds(0), 3});
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(3, 2s));
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "cancelOrder", "order"}));
}

// The command handle registered with a mock RMF adapter, so operator controls and removal have an update handle.
class RmfCommandHandleTest : public CommandHandleBrokerTest
{
protected:
    static void SetUpTestSuite()
    {
        if (!broker_url() || rclcpp::ok())
        {
            return;
        }
        setenv("ROS_AUTOMATIC_DISCOVERY_RANGE", "LOCALHOST", 1);
        rclcpp::InitOptions options;
        options.set_domain_id(kRosDomain);
        rclcpp::init(0, nullptr, options);
    }

    static void TearDownTestSuite()
    {
        if (rclcpp::ok())
        {
            rclcpp::shutdown();
        }
    }

    void SetUp() override
    {
        CommandHandleBrokerTest::SetUp();
        if (IsSkipped() || HasFatalFailure())
        {
            return;
        }
        adapter = std::make_shared<rmf_fleet_adapter::agv::test::MockAdapter>("vda5050_review_test");
        fleet = adapter->add_fleet("review_fleet", traits(), *graph);
        // A task planner, as the adapter always sets one up.
        const auto battery = std::make_shared<rmf_battery::agv::BatterySystem>(*rmf_battery::agv::BatterySystem::make(24.0, 40.0, 8.8));
        const auto mechanical = rmf_battery::agv::MechanicalSystem::make(70.0, 40.0, 0.22);
        const auto ambient = rmf_battery::agv::PowerSystem::make(20.0);
        ASSERT_TRUE(fleet->set_task_planner_params(
            battery, std::make_shared<rmf_battery::agv::SimpleMotionPowerSink>(*battery, *mechanical),
            std::make_shared<rmf_battery::agv::SimpleDevicePowerSink>(*battery, *ambient),
            std::make_shared<rmf_battery::agv::SimpleDevicePowerSink>(*battery, *ambient), 0.2, 1.0, false));
        adapter->start();
        handle = make_handle(true);
        auto starts = rmf_traffic::agv::compute_plan_starts(*graph, kMap, {0.0, 0.0, 0.0}, std::chrono::steady_clock::now());
        auto added = std::make_shared<std::promise<std::shared_ptr<rmf_fleet_adapter::agv::RobotUpdateHandle>>>();
        const auto command = handle;
        fleet->add_robot(handle, "r1", traits().profile(), starts,
                         [command, added](const std::shared_ptr<rmf_fleet_adapter::agv::RobotUpdateHandle> &update)
                         {
                             command->set_update_handle(update);
                             added->set_value(update);
                         });
        auto future = added->get_future();
        ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
        update_handle = future.get();
    }

    // Whether RMF may give the robot tasks, once RMF has applied the latest commission.
    bool accepting_tasks()
    {
        std::this_thread::sleep_for(300ms);
        return update_handle->commission().is_accepting_dispatched_tasks();
    }

    void TearDown() override
    {
        if (adapter)
        {
            adapter->stop();
        }
        fleet.reset();
        adapter.reset();
        CommandHandleBrokerTest::TearDown();
    }

    std::shared_ptr<rmf_fleet_adapter::agv::test::MockAdapter> adapter;
    std::shared_ptr<rmf_fleet_adapter::agv::FleetUpdateHandle> fleet;
    std::shared_ptr<rmf_fleet_adapter::agv::RobotUpdateHandle> update_handle;
};

TEST_F(RmfCommandHandleTest, AnOperatorPauseOutlastsATrafficHold)
{
    const auto to_c = plan(*graph, 2);
    handle->follow_new_path(to_c, kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    EXPECT_EQ(handle->pause(), "");
    // The AGV reports the pause a moment after it was asked for.
    handle->update(*connector->get_data("r1"));
    acknowledge(0, 3, true);
    handle->update(*connector->get_data("r1"));
    handle->stop();
    handle->follow_new_path(to_c, kNoEstimate, kNoCallback);
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause"})) << "the hold finds the AGV paused by the operator";
}

TEST_F(RmfCommandHandleTest, AResumeDuringATrafficHoldWaitsForTheNewPath)
{
    const auto to_c = plan(*graph, 2);
    handle->follow_new_path(to_c, kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    EXPECT_EQ(handle->pause(), "");
    handle->stop();
    EXPECT_EQ(handle->resume(), "");
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause"}));
    acknowledge(0, 3, true);
    handle->follow_new_path(to_c, kNoEstimate, kNoCallback);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause", "stopPause"}));
}

TEST_F(RmfCommandHandleTest, TheAgvStaysAvailableWhileItUnpausesAfterATrafficHold)
{
    handle->set_online(true);
    const auto to_c = plan(*graph, 2);
    handle->follow_new_path(to_c, kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->update(*connector->get_data("r1"));
    ASSERT_TRUE(accepting_tasks());

    handle->stop();
    acknowledge(0, 3, true);
    handle->update(*connector->get_data("r1"));
    handle->follow_new_path(to_c, kNoEstimate, kNoCallback);
    // The AGV still reports the pause until it has handled stopPause.
    handle->update(*connector->get_data("r1"));
    EXPECT_TRUE(accepting_tasks());
    acknowledge(0, 4);
    handle->update(*connector->get_data("r1"));
    EXPECT_TRUE(accepting_tasks());
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause", "stopPause"}));
}

TEST_F(RmfCommandHandleTest, AnAgvPausedByOthersIsDecommissioned)
{
    handle->set_online(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->update(*connector->get_data("r1"));
    ASSERT_TRUE(accepting_tasks());
    acknowledge(0, 3, true);
    handle->update(*connector->get_data("r1"));
    EXPECT_FALSE(accepting_tasks());
    acknowledge(0, 4);
    handle->update(*connector->get_data("r1"));
    EXPECT_TRUE(accepting_tasks());
}

TEST_F(RmfCommandHandleTest, AnOperatorPauseKeepsTheRobotUnavailableThroughATrafficHold)
{
    handle->set_online(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->update(*connector->get_data("r1"));
    ASSERT_TRUE(accepting_tasks());
    EXPECT_EQ(handle->pause(), "");
    acknowledge(0, 3, true);
    handle->update(*connector->get_data("r1"));
    EXPECT_FALSE(accepting_tasks());
    handle->stop();
    handle->update(*connector->get_data("r1"));
    EXPECT_FALSE(accepting_tasks()) << "a traffic hold does not hide the operator's pause";
}

TEST_F(RmfCommandHandleTest, ARetiredRobotTakesNoCommandsUntilItIsRestored)
{
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->retire();
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    handle->stop();
    handle->update(*connector->get_data("r1"));
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "cancelOrder"}));

    handle->restore();
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    answer_cancel(1, 3);
    handle->update(*connector->get_data("r1"));
    capture->wait_for(3, 2s);
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "cancelOrder", "order"}));
}

TEST_F(RmfCommandHandleTest, UpdatesReportProgressOnAPathAndWhileIdle)
{
    int estimates = 0;
    bool finished = false;
    handle->follow_new_path(plan(*graph, 2), [&estimates](std::size_t, rmf_traffic::Duration) { ++estimates; },
                            [&finished]() { finished = true; });
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    for (int i = 0; i < 5; ++i)
    {
        handle->update(*connector->get_data("r1"));
    }
    EXPECT_GT(estimates, 0);
    EXPECT_FALSE(finished);

    finish(0, 3);
    handle->update(*connector->get_data("r1"));
    EXPECT_TRUE(finished);
    for (int i = 0; i < 5; ++i)
    {
        handle->update(*connector->get_data("r1"));
    }
    EXPECT_EQ(capture->kinds(), Kinds{"order"});
}

TEST_F(RmfCommandHandleTest, CommandsUpdatesAndOperatorControlsRunTogetherWithoutDeadlock)
{
    const auto to_c = plan(*graph, 2);
    const auto to_d = plan(*graph, 3);
    std::atomic<bool> running{true};
    std::atomic<int> updates{0};
    std::atomic<int> commands{0};

    std::thread update_loop([&]
    {
        int header = 100;
        while (running)
        {
            ++header;
            feed(agv_state(header, "", "", std::nullopt, nlohmann::json::array(), header % 3 == 0));
            if (const auto data = connector->get_data("r1"))
            {
                handle->update(*data);
            }
            handle->expire_traffic_hold();
            ++updates;
        }
    });
    std::thread operator_controls([&]
    {
        while (running)
        {
            handle->pause();
            handle->resume();
        }
    });
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < until)
    {
        handle->follow_new_path(to_c, kNoEstimate, kNoCallback);
        handle->stop();
        handle->follow_new_path(to_d, kNoEstimate, kNoCallback);
        commands += 3;
    }
    running = false;
    update_loop.join();
    operator_controls.join();
    EXPECT_GT(updates.load(), 100);
    EXPECT_GT(commands.load(), 30);
}

TEST_F(RmfCommandHandleTest, ARefusedOrderIsDroppedAndNotSentAgain)
{
    bool finished = false;
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, [&finished]() { finished = true; });
    ASSERT_TRUE(capture->wait_for(1, 3s));
    const std::string order_id = capture->message(0)["orderId"];
    auto refused = agv_state(2, "", "", std::nullopt, nlohmann::json::array(), false);
    refused["errors"] = nlohmann::json::array({{{"errorType", "orderError"}, {"errorLevel", "WARNING"},
                                                {"errorReferences", {{{"referenceKey", "orderId"}, {"referenceValue", order_id}}}}}});
    feed(refused);
    EXPECT_EQ(connector->refusal_of("r1", order_id), std::optional<std::string>("orderError"));
    handle->update(*connector->get_data("r1"));

    feed(agv_state(3, order_id, "C", 2, nlohmann::json::array(), false));
    handle->update(*connector->get_data("r1"));
    std::this_thread::sleep_for(300ms);
    EXPECT_FALSE(finished);
    EXPECT_EQ(capture->kinds(), Kinds{"order"});
}

TEST_F(RmfCommandHandleTest, ChargingStartsAtAChargerAndStopsBeforeTheNextOrder)
{
    vda5050_fleet_adapter_full_control::rmf::ActionPolicy policy;
    policy.charge_at_chargers = true;
    handle->set_action_policy(policy);

    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    finish(0, 2);
    handle->update(*connector->get_data("r1"));
    ASSERT_TRUE(capture->wait_for(2, 3s));
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startCharging"}));

    auto charging = agv_state(3, capture->message(0)["orderId"], "C", 2, nlohmann::json::array(), false);
    charging["batteryState"]["charging"] = true;
    feed(charging);
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(3, 2s));
    handle->update(*connector->get_data("r1"));
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startCharging", "stopCharging"})) << "the order waits for the AGV to stop charging";

    auto stopped = agv_state(4, capture->message(0)["orderId"], "C", 2, nlohmann::json::array(), false);
    stopped["actionStates"] = nlohmann::json::array({{{"actionId", capture->message(2)["actions"][0]["actionId"]},
                                                      {"actionType", "stopCharging"}, {"actionStatus", "FINISHED"}}});
    feed(stopped);
    handle->update(*connector->get_data("r1"));
    capture->wait_for(4, 2s);
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startCharging", "stopCharging", "order"}));
}

TEST_F(RmfCommandHandleTest, NoChargingActionsWithoutTheSetting)
{
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    finish(0, 2);
    handle->update(*connector->get_data("r1"));
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), Kinds{"order"});
}

TEST_F(RmfCommandHandleTest, NoStopChargingForAnAgvThatIsNotCharging)
{
    vda5050_fleet_adapter_full_control::rmf::ActionPolicy policy;
    policy.charge_at_chargers = true;
    handle->set_action_policy(policy);

    handle->follow_new_path(plan(*graph, 1), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    finish(0, 2);
    handle->update(*connector->get_data("r1"));
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    capture->wait_for(2, 2s);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "order"})) << "B is not a charger and the AGV reports no charging";
}

// r1 maps level L1 to floor_1 and level L2 to floor_2, whose frame is shifted 10 m along x.
class MultiLevelTest : public CommandHandleBrokerTest
{
protected:
    void register_robot() override
    {
        LevelMaps maps;
        maps.set(kMap, {"floor_1", Transform()});
        maps.set("L2", {"floor_2", Transform(0.0, 1.0, 10.0, 0.0)});
        connector->add_robot("r1", "M", "S1", maps);
    }

    // State at x on `map_id`, with both floors reported.
    static nlohmann::json state_on(int header, const std::string &map_id, double x)
    {
        auto s = agv_state(header, "", "", std::nullopt, nlohmann::json::array(), false);
        s["maps"] = nlohmann::json::array({{{"mapId", "floor_1"}, {"mapStatus", "ENABLED"}}, {{"mapId", "floor_2"}, {"mapStatus", "DISABLED"}}});
        s["agvPosition"]["mapId"] = map_id;
        s["agvPosition"]["x"] = x;
        return s;
    }

    static std::map<std::string, nlohmann::json> parameters_of(const nlohmann::json &message)
    {
        std::map<std::string, nlohmann::json> out;
        for (const auto &p : message["actions"][0]["actionParameters"])
        {
            out[p["key"].get<std::string>()] = p["value"];
        }
        return out;
    }
};

TEST_F(MultiLevelTest, TheAgvMapIsReportedAsItsLevelInTheLevelFrame)
{
    feed(state_on(2, "floor_2", 12.0));
    const auto data = connector->get_data("r1");
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(data->map_name, "L2");
    EXPECT_NEAR(data->position[0], 2.0, 1e-9);
    EXPECT_EQ(connector->get_known_map("r1"), std::optional<std::string>("L2"));
}

TEST_F(MultiLevelTest, OrderNodesCarryTheMapIdAndFrameOfTheirLevel)
{
    feed(state_on(2, "floor_1", 0.0));
    const std::vector<Connector::RoutePoint> route = {{"C", 4.0, 0.0, 0.0, std::nullopt, kMap},
                                                      {"C2", 4.0, 0.0, 0.0, std::nullopt, "L2"},
                                                      {"E2", 6.0, 0.0, 0.0, std::nullopt, "L2"}};
    ASSERT_EQ(connector->navigate_route("r1", route, kMap).status, CommandStatus::queued);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    const auto nodes = capture->message(0)["nodes"];
    ASSERT_EQ(nodes.size(), 4u);
    const std::vector<std::string> maps = {"floor_1", "floor_1", "floor_2", "floor_2"};
    const std::vector<double> xs = {0.0, 4.0, 14.0, 16.0};
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        EXPECT_EQ(nodes[i]["nodePosition"]["mapId"], maps[i]) << i;
        EXPECT_NEAR(nodes[i]["nodePosition"]["x"].get<double>(), xs[i], 1e-9) << i;
    }
}

TEST_F(MultiLevelTest, InitPositionUsesTheMapIdAndFrameOfTheLevel)
{
    ASSERT_FALSE(connector->init_position("r1", 2.0, 0.0, 0.0, "L2", "C2").empty());
    ASSERT_TRUE(capture->wait_for(1, 3s));
    auto parameters = parameters_of(capture->message(0));
    EXPECT_EQ(parameters["mapId"], "floor_2");
    EXPECT_NEAR(parameters["x"].get<double>(), 12.0, 1e-9);
}

TEST_F(MultiLevelTest, APathAcrossLevelsKeepsEachNodeOnItsLevel)
{
    const auto c2 = graph->add_waypoint("L2", {4.0, 0.0}).index();
    graph->add_key("C2", c2);
    const auto e2 = graph->add_waypoint("L2", {4.0, 2.0}).index();
    graph->add_key("E2", e2);
    for (const auto &[a, b] : std::vector<std::pair<std::size_t, std::size_t>>{{2, c2}, {c2, e2}})
    {
        graph->add_lane(a, b);
        graph->add_lane(b, a);
    }
    feed(state_on(2, "floor_1", 0.0));
    handle = make_handle(true);
    const auto path = plan(*graph, e2);
    ASSERT_FALSE(path.empty());
    handle->follow_new_path(path, kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    const auto nodes = capture->message(0)["nodes"];
    ASSERT_GE(nodes.size(), 2u);
    EXPECT_EQ(nodes.front()["nodePosition"]["mapId"], "floor_1") << "the base node is where the AGV is";
    EXPECT_EQ(nodes.back()["nodeId"], "E2");
    for (std::size_t i = 1; i < nodes.size(); ++i)
    {
        const auto *wp = graph->find_waypoint(nodes[i]["nodeId"].get<std::string>());
        ASSERT_NE(wp, nullptr) << nodes[i]["nodeId"];
        const bool upper = wp->get_map_name() == "L2";
        EXPECT_EQ(nodes[i]["nodePosition"]["mapId"], upper ? "floor_2" : "floor_1") << nodes[i]["nodeId"];
        EXPECT_NEAR(nodes[i]["nodePosition"]["x"].get<double>(), wp->get_location().x() + (upper ? 10.0 : 0.0), 1e-9) << nodes[i]["nodeId"];
    }
}

// Needs a broker from fleet_bringup/broker/setup_broker.py with ROBOTIS/0001 on interface AMR.
TEST(MqttTls, MasterAndAgvExchangeMessagesOverTlsWithTheirOwnAccounts)
{
    const char *broker = std::getenv("VDA5050_TEST_TLS_BROKER");
    const char *ca = std::getenv("VDA5050_TEST_TLS_CA");
    const char *master_password = std::getenv("VDA5050_TEST_TLS_MASTER_PASSWORD");
    const char *agv_password = std::getenv("VDA5050_TEST_TLS_AGV_PASSWORD");
    if (!broker || !ca || !master_password || !agv_password)
    {
        GTEST_SKIP() << "set VDA5050_TEST_TLS_BROKER, _CA, _MASTER_PASSWORD and _AGV_PASSWORD to run this test";
    }
    using vda5050_fleet_adapter_full_control::mqtt::MqttClient;
    vda5050_fleet_adapter_full_control::mqtt::MqttOptions options;
    options.tls.enabled = true;
    options.tls.ca_file = ca;
    MqttClient master(broker, "tls_master_check", std::string("fleet_master"), std::string(master_password), options);
    MqttClient agv(broker, "tls_agv_check", std::string("ROBOTIS_0001"), std::string(agv_password), options);
    std::mutex mutex;
    std::vector<std::string> received;
    const auto record = [&](const std::string &topic, const std::string &) {
        std::lock_guard<std::mutex> lock(mutex);
        received.push_back(topic);
    };
    master.set_on_message(record);
    agv.set_on_message(record);
    master.set_on_connected([&master]() { master.subscribe("AMR/v2/ROBOTIS/0001/state", 0); });
    agv.set_on_connected([&agv]() { agv.subscribe("AMR/v2/ROBOTIS/0001/order", 0); });
    master.connect();
    agv.connect();
    for (int i = 0; i < 100 && !(master.is_connected() && agv.is_connected()); ++i)
    {
        std::this_thread::sleep_for(50ms);
    }
    ASSERT_TRUE(master.is_connected() && agv.is_connected());
    std::this_thread::sleep_for(300ms);

    EXPECT_TRUE(master.publish("AMR/v2/ROBOTIS/0001/order", "{}", 0));
    EXPECT_TRUE(agv.publish("AMR/v2/ROBOTIS/0001/state", "{}", 0));
    EXPECT_TRUE(agv.publish("AMR/v2/ROBOTIS/0001/order", "{}", 0)) << "queued; the broker drops it";
    std::this_thread::sleep_for(800ms);
    master.shutdown();
    agv.shutdown();
    std::lock_guard<std::mutex> lock(mutex);
    std::sort(received.begin(), received.end());
    EXPECT_EQ(received, (std::vector<std::string>{"AMR/v2/ROBOTIS/0001/order", "AMR/v2/ROBOTIS/0001/state"}));
}

TEST(MqttReconnect, ConnectsWhenTheBrokerStartsAfterTheAdapter)
{
    const char *mosquitto = std::getenv("VDA5050_TEST_MOSQUITTO");
    if (!mosquitto || !*mosquitto)
    {
        GTEST_SKIP() << "set VDA5050_TEST_MOSQUITTO to a mosquitto binary to run this test";
    }
    const std::string port = "18839";
    vda5050_fleet_adapter_full_control::mqtt::MqttClient client("tcp://127.0.0.1:" + port, "review_late_broker");
    client.connect();
    std::this_thread::sleep_for(1500ms);
    ASSERT_FALSE(client.is_connected());

    pid_t pid = 0;
    char *argv[] = {const_cast<char *>(mosquitto), const_cast<char *>("-p"), const_cast<char *>(port.c_str()), nullptr};
    ASSERT_EQ(posix_spawn(&pid, mosquitto, nullptr, nullptr, argv, environ), 0);
    bool connected = false;
    for (int i = 0; i < 400 && !connected; ++i)
    {
        std::this_thread::sleep_for(50ms);
        connected = client.is_connected();
    }
    client.shutdown();
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    EXPECT_TRUE(connected) << "no connection 20 s after the broker came up";
}
