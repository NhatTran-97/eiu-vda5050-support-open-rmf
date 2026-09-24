#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
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
#include "vda5050_fleet_adapter_full_control/vda5050/route_stitch.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/state_handler.hpp"

using vda5050_fleet_adapter_full_control::rmf::Connector;
using vda5050_fleet_adapter_full_control::rmf::Transform;
using vda5050_fleet_adapter_full_control::rmf::VdaRobotCommandHandle;
namespace vda = vda5050_fleet_adapter_full_control::vda5050;
using Clock = std::chrono::steady_clock;

namespace {

double micros(Clock::duration d)
{
    return std::chrono::duration<double, std::micro>(d).count();
}

double percentile(std::vector<double> values, double fraction)
{
    if (values.empty())
    {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(values.size())))];
}

// A state as a busy AGV sends it: a 30-node order, running actions, errors, information, a load and a map.
nlohmann::json busy_state(int header)
{
    using nlohmann::json;
    json nodes = json::array();
    json edges = json::array();
    for (int i = 1; i <= 30; ++i)
    {
        nodes.push_back({{"nodeId", "wp" + std::to_string(i)}, {"sequenceId", 2 * i}, {"released", i < 20},
                         {"nodePosition", {{"x", i * 1.0}, {"y", 0.5}, {"theta", 0.0}, {"mapId", "map"}}}});
        edges.push_back({{"edgeId", "e" + std::to_string(i)}, {"sequenceId", 2 * i - 1}, {"released", i < 20}});
    }
    json actions = json::array();
    for (int i = 0; i < 10; ++i)
    {
        actions.push_back({{"actionId", "a" + std::to_string(i)}, {"actionType", "pick"}, {"actionStatus", i < 5 ? "FINISHED" : "RUNNING"}});
    }
    return {{"headerId", header}, {"timestamp", "2026-09-23T10:00:00.000Z"}, {"version", "2.1.0"},
            {"manufacturer", "M"}, {"serialNumber", "S"}, {"orderId", "order-1"}, {"orderUpdateId", 3},
            {"lastNodeId", "wp0"}, {"lastNodeSequenceId", 0}, {"driving", true}, {"paused", false},
            {"newBaseRequest", false}, {"distanceSinceLastNode", 0.4},
            {"nodeStates", nodes}, {"edgeStates", edges}, {"actionStates", actions},
            {"errors", json::array({{{"errorType", "warn1"}, {"errorLevel", "WARNING"}, {"errorDescription", "low battery soon"}}})},
            {"information", json::array({{{"infoType", "i"}, {"infoLevel", "INFO"}}})},
            {"loads", json::array({{{"loadId", "L1"}, {"loadType", "box"}}})},
            {"maps", json::array({{{"mapId", "map"}, {"mapStatus", "ENABLED"}}})},
            {"operatingMode", "AUTOMATIC"}, {"safetyState", {{"eStop", "NONE"}, {"fieldViolation", false}}},
            {"batteryState", {{"batteryCharge", 80.0}, {"charging", false}}},
            {"velocity", {{"vx", 0.3}, {"vy", 0.0}, {"omega", 0.1}}},
            {"agvPosition", {{"x", 1.0}, {"y", 2.0}, {"theta", 0.0}, {"mapId", "map"}, {"positionInitialized", true}}}};
}

// A square grid of waypoints 2 m apart with lanes both ways between neighbours.
std::shared_ptr<rmf_traffic::agv::Graph> grid(std::size_t side)
{
    auto graph = std::make_shared<rmf_traffic::agv::Graph>();
    for (std::size_t r = 0; r < side; ++r)
    {
        for (std::size_t c = 0; c < side; ++c)
        {
            graph->add_waypoint("L1", {2.0 * static_cast<double>(c), 2.0 * static_cast<double>(r)});
            graph->add_key("wp_" + std::to_string(r) + "_" + std::to_string(c), r * side + c);
        }
    }
    for (std::size_t r = 0; r < side; ++r)
    {
        for (std::size_t c = 0; c < side; ++c)
        {
            const std::size_t here = r * side + c;
            if (c + 1 < side)
            {
                graph->add_lane(here, here + 1);
                graph->add_lane(here + 1, here);
            }
            if (r + 1 < side)
            {
                graph->add_lane(here, here + side);
                graph->add_lane(here + side, here);
            }
        }
    }
    return graph;
}

}  // namespace

TEST(Performance, StateMessageCostByStage)
{
    Connector connector(rclcpp::get_logger("perf"), "tcp://localhost:1", "AMR");
    connector.add_robot("r1", "M", "S", Transform());
    constexpr int kMessages = 20000;
    std::vector<std::string> payloads;
    for (int i = 0; i < kMessages; ++i)
    {
        payloads.push_back(busy_state(i + 1).dump());
    }

    auto start = Clock::now();
    for (const auto &p : payloads)
    {
        const auto raw = nlohmann::json::parse(p);
        (void)raw;
    }
    const double parse_us = micros(Clock::now() - start) / kMessages;

    const auto raw = nlohmann::json::parse(payloads.front());
    start = Clock::now();
    for (int i = 0; i < kMessages; ++i)
    {
        const vda::ParsedState parsed(raw);
        (void)parsed;
    }
    const double model_us = micros(Clock::now() - start) / kMessages;

    start = Clock::now();
    for (const auto &p : payloads)
    {
        connector.handle_message("AMR/v2/M/S/state", p);
    }
    const double total_us = micros(Clock::now() - start) / kMessages;

    std::printf("[perf] state message (%zu bytes): parse %.1f us, ParsedState %.1f us, handle_message %.1f us "
                "-> %.0f msg/s on one core\n",
                payloads.front().size(), parse_us, model_us, total_us, 1e6 / total_us);
    EXPECT_TRUE(connector.is_online("r1"));
    EXPECT_LT(total_us, 1000.0);
}

// The update loop and the MQTT thread share one lock; this measures how long an update pass takes while states pour in.
TEST(Performance, UpdatePassUnderAStateFlood)
{
    constexpr int kRobots = 100;
    Connector connector(rclcpp::get_logger("perf"), "tcp://localhost:1", "AMR");
    connector.set_stale_state_streak(0);
    std::vector<std::string> topics;
    for (int i = 0; i < kRobots; ++i)
    {
        const std::string serial = "S" + std::to_string(i);
        connector.add_robot("r" + std::to_string(i), "M", serial, Transform());
        topics.push_back("AMR/v2/M/" + serial + "/state");
    }
    std::vector<std::string> payloads;
    for (int i = 0; i < 64; ++i)
    {
        payloads.push_back(busy_state(i + 1).dump());
    }

    const auto run_passes = [&](Clock::duration length)
    {
        std::vector<double> passes;
        const auto until = Clock::now() + length;
        while (Clock::now() < until)
        {
            const auto begin = Clock::now();
            for (int i = 0; i < kRobots; ++i)
            {
                const std::string name = "r" + std::to_string(i);
                if (connector.is_online(name))
                {
                    connector.poll(name);
                    (void)connector.get_data(name);
                    (void)connector.is_command_completed(name);
                    (void)connector.is_order_stuck(name);
                }
            }
            passes.push_back(micros(Clock::now() - begin));
        }
        return passes;
    };

    for (int i = 0; i < kRobots; ++i)
    {
        connector.handle_message(topics[static_cast<std::size_t>(i)], payloads[0]);
    }
    const auto quiet = run_passes(std::chrono::milliseconds(500));

    std::atomic<bool> flooding{true};
    std::atomic<long> delivered{0};
    std::thread mqtt([&]
    {
        std::size_t n = 0;
        while (flooding)
        {
            const std::size_t robot = n % kRobots;
            connector.handle_message(topics[robot], payloads[n % payloads.size()]);
            ++n;
            ++delivered;
        }
    });
    const auto busy = run_passes(std::chrono::seconds(2));
    flooding = false;
    mqtt.join();

    std::printf("[perf] update pass over %d robots: quiet p50 %.0f us p99 %.0f us; under flood p50 %.0f us p99 %.0f us "
                "max %.0f us; flood delivered %.0f msg/s\n",
                kRobots, percentile(quiet, 0.5), percentile(quiet, 0.99), percentile(busy, 0.5), percentile(busy, 0.99),
                percentile(busy, 1.0), static_cast<double>(delivered.load()) / 2.0);
    EXPECT_LT(percentile(busy, 0.99), 100000.0);
}

// RMF runs compute_plan_starts for every update_position(map, position) call; this is its cost by graph size.
TEST(Performance, ComputePlanStartsCostByGraphSize)
{
    for (const std::size_t side : {10u, 30u, 60u})
    {
        const auto graph = grid(side);
        const Eigen::Vector3d on_lane(3.0, 2.05, 0.0);
        const auto now = Clock::now();
        constexpr int kCalls = 2000;
        std::size_t starts = 0;
        const auto begin = Clock::now();
        for (int i = 0; i < kCalls; ++i)
        {
            starts += rmf_traffic::agv::compute_plan_starts(*graph, "L1", on_lane, now).size();
        }
        const double us = micros(Clock::now() - begin) / kCalls;
        std::printf("[perf] compute_plan_starts: %zu waypoints, %zu lanes: %.1f us per call (%zu starts); "
                    "50 robots at 10 Hz = %.1f%% of RMF's worker thread\n",
                    graph->num_waypoints(), graph->num_lanes(), us, starts / kCalls, us * 50 * 10 / 1e4);
        EXPECT_GT(starts, 0u);
    }
}

// follow_new_path runs on RMF's worker thread; this is the adapter's own work for a path with many turns.
TEST(Performance, FollowNewPathOnALargeGraph)
{
    // A 60x60 grid, and beside it a staircase whose every waypoint is a turn.
    const auto graph = grid(60);
    constexpr std::size_t kSteps = 60;
    const std::size_t first = graph->num_waypoints();
    for (std::size_t i = 0; i <= 2 * kSteps; ++i)
    {
        const double x = 200.0 + 2.0 * static_cast<double>((i + 1) / 2);
        const double y = 2.0 * static_cast<double>(i / 2);
        graph->add_waypoint("L1", {x, y});
        graph->add_key("st_" + std::to_string(i), first + i);
        if (i > 0)
        {
            graph->add_lane(first + i - 1, first + i);
            graph->add_lane(first + i, first + i - 1);
        }
    }
    const rmf_traffic::Profile profile{rmf_traffic::geometry::make_final_convex<rmf_traffic::geometry::Circle>(0.2)};
    rmf_traffic::agv::Planner planner{
        rmf_traffic::agv::Planner::Configuration{*graph, rmf_traffic::agv::VehicleTraits{{0.5, 0.7}, {0.6, 1.5}, profile}},
        rmf_traffic::agv::Planner::Options{nullptr}};
    const auto result = planner.plan(rmf_traffic::agv::Planner::Start{Clock::now(), first, 0.0},
                                     rmf_traffic::agv::Planner::Goal{first + 2 * kSteps});
    ASSERT_TRUE(result.success());
    const auto &waypoints = result->get_waypoints();

    Connector connector(rclcpp::get_logger("perf"), "tcp://localhost:1", "AMR");
    connector.add_robot("r1", "M", "S", Transform());
    nlohmann::json s = busy_state(1);
    s["maps"] = nlohmann::json::array({{{"mapId", "L1"}, {"mapStatus", "ENABLED"}}});
    s["agvPosition"] = {{"x", 200.0}, {"y", 0.0}, {"theta", 0.0}, {"mapId", "L1"}, {"positionInitialized", true}};
    connector.handle_message("AMR/v2/M/S/state", s.dump());
    auto handle = std::make_shared<VdaRobotCommandHandle>(rclcpp::get_logger("perf"), "r1", connector, graph, 0.5,
                                                          std::make_shared<rclcpp::Clock>(RCL_STEADY_TIME));

    constexpr int kCalls = 20;
    const auto begin = Clock::now();
    for (int i = 0; i < kCalls; ++i)
    {
        handle->follow_new_path(waypoints, [](std::size_t, rmf_traffic::Duration) {}, [] {});
    }
    const double us = micros(Clock::now() - begin) / kCalls;
    std::printf("[perf] follow_new_path: %zu waypoints on a graph of %zu lanes: %.0f us per call\n", waypoints.size(),
                graph->num_lanes(), us);

    std::size_t found = 0;
    const auto lookup = Clock::now();
    for (int i = 0; i < kCalls; ++i)
    {
        for (std::size_t k = 1; k < waypoints.size(); ++k)
        {
            const auto from = waypoints[k - 1].graph_index();
            const auto to = waypoints[k].graph_index();
            if (from && to && *from != *to)
            {
                found += graph->lane_from(*from, *to) ? 1 : 0;
            }
        }
    }
    std::printf("[perf] the same lane checks through Graph::lane_from: %.1f us per path (%zu lanes found)\n",
                micros(Clock::now() - lookup) / kCalls, found / kCalls);
}

TEST(Performance, StitchPlanningOnLongRoutes)
{
    std::vector<vda::RouteWaypoint> route;
    for (int i = 0; i < 500; ++i)
    {
        route.push_back({"wp" + std::to_string(i), {1.0 * i, 0.0, 0.0}, std::nullopt});
    }
    const std::vector<vda::RouteWaypoint> replanned(route.begin() + 100, route.end());
    constexpr int kCalls = 2000;
    const auto begin = Clock::now();
    std::size_t ok = 0;
    for (int i = 0; i < kCalls; ++i)
    {
        ok += vda::plan_stitch(route, 400, 100, replanned).has_value() ? 1 : 0;
    }
    std::printf("[perf] plan_stitch over a 500-point route: %.1f us per call\n", micros(Clock::now() - begin) / kCalls);
    EXPECT_EQ(ok, static_cast<std::size_t>(kCalls));
}
