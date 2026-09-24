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

extern char **environ;

namespace {

using namespace std::chrono_literals;
using vda5050_fleet_adapter_full_control::rmf::CommandStatus;
using vda5050_fleet_adapter_full_control::rmf::Connector;
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

private:
    mqtt::async_client _client;
    std::mutex _mutex;
    std::condition_variable _cv;
    std::vector<nlohmann::json> _messages;
    std::vector<std::string> _kinds;
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

// A(0,0) - B(2,0) - C(4,0), with D(2,2) off B; every lane runs both ways and A is a charger.
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
        connector->add_robot("r1", "M", "S1", Transform());
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

    void TearDown() override
    {
        handle.reset();
        capture.reset();
        if (connector)
        {
            connector->shutdown();
        }
    }

    std::shared_ptr<VdaRobotCommandHandle> make_handle(bool stitch, double traffic_pause_timeout_s = 10.0)
    {
        RoutePolicy policy;
        policy.traffic_pause_timeout_s = traffic_pause_timeout_s;
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

    // B and C of the graph as connector route points.
    static std::vector<Connector::RoutePoint> route_to_c()
    {
        return {{"B", 2.0, 0.0, 0.0, std::nullopt}, {"C", 4.0, 0.0, 0.0, std::nullopt}};
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
    capture->wait_for(3, 1s);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (std::vector<std::string>{"order", "startPause", "stopPause"}));
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

TEST_F(CommandHandleBrokerTest, AHeldOrderIsCancelledBeforeADifferentRouteReplacesIt)
{
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->stop();
    ASSERT_TRUE(capture->wait_for(2, 3s));
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    capture->wait_for(5, 2s);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause", "cancelOrder", "order", "stopPause"}));
}

TEST_F(CommandHandleBrokerTest, ASupersededOrderIsCancelledBeforeItsReplacement)
{
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
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
    // Four nodes, over the AGV's limit of three.
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    std::this_thread::sleep_for(500ms);
    handle->expire_traffic_hold();
    std::this_thread::sleep_for(300ms);
    handle->expire_traffic_hold();
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause", "cancelOrder", "stopPause"}));
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

// Replacing an order sends the new one only once the AGV has answered the cancelOrder.
TEST_F(CommandHandleBrokerTest, DISABLED_ANewOrderWaitsForTheCancelToBeConfirmed)
{
    handle = make_handle(true);
    handle->follow_new_path(plan(*graph, 2), kNoEstimate, kNoCallback);
    ASSERT_TRUE(capture->wait_for(1, 3s));
    acknowledge(0, 2);
    handle->follow_new_path(plan(*graph, 3), kNoEstimate, kNoCallback);
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(capture->kinds(), (std::vector<std::string>{"order", "cancelOrder"}));
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
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause", "startPause"}));
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
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause", "startPause"}));
    handle->follow_new_path(to_c, kNoEstimate, kNoCallback);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(capture->kinds(), (Kinds{"order", "startPause", "startPause", "stopPause"}));
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
