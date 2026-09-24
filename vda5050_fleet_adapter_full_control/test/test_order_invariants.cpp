#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "vda5050_fleet_adapter_full_control/vda5050/order_handler.hpp"
#include "vda5050_fleet_adapter_full_control/vda5050/route_stitch.hpp"

namespace vda = vda5050_fleet_adapter_full_control::vda5050;

namespace {

constexpr int kIterations = 3000;

// A route along x with 1 m steps, small sideways jitter and occasional turns in place.
std::vector<vda::RouteWaypoint> random_route(std::mt19937 &rng, std::size_t points, int first_id, double start_x)
{
    std::uniform_real_distribution<double> jitter(-0.2, 0.2);
    std::uniform_real_distribution<double> heading(-3.0, 3.0);
    std::bernoulli_distribution turn(0.15);
    std::vector<vda::RouteWaypoint> route;
    double x = start_x;
    int id = first_id;
    while (route.size() < points)
    {
        x += 1.0;
        const vda::RouteWaypoint wp{"wp" + std::to_string(id++), {x, jitter(rng), heading(rng)}, std::nullopt};
        route.push_back(wp);
        if (route.size() < points && turn(rng))
        {
            vda::RouteWaypoint again = wp;
            again.pose.theta = heading(rng);
            route.push_back(again);
        }
    }
    return route;
}

// VDA5050 structure: alternating sequence IDs from `first_sequence`, edges joining their nodes, base before horizon.
void expect_well_formed(const nlohmann::json &order, int first_sequence)
{
    const auto &nodes = order.at("nodes");
    const auto &edges = order.at("edges");
    ASSERT_FALSE(nodes.empty());
    ASSERT_EQ(edges.size() + 1, nodes.size());
    EXPECT_TRUE(nodes[0].at("released").get<bool>()) << "the first node must be released";
    bool horizon = false;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        EXPECT_EQ(nodes[i].at("sequenceId").get<int>(), first_sequence + 2 * static_cast<int>(i));
        const bool released = nodes[i].at("released").get<bool>();
        EXPECT_FALSE(horizon && released) << "released node after the horizon started, index " << i;
        horizon = horizon || !released;
    }
    for (std::size_t i = 0; i < edges.size(); ++i)
    {
        EXPECT_EQ(edges[i].at("sequenceId").get<int>(), first_sequence + 2 * static_cast<int>(i) + 1);
        EXPECT_EQ(edges[i].at("startNodeId"), nodes[i].at("nodeId"));
        EXPECT_EQ(edges[i].at("endNodeId"), nodes[i + 1].at("nodeId"));
        EXPECT_EQ(edges[i].at("released").get<bool>(), nodes[i + 1].at("released").get<bool>());
    }
}

std::size_t released_nodes(const nlohmann::json &order)
{
    std::size_t count = 0;
    for (const auto &n : order.at("nodes"))
    {
        count += n.at("released").get<bool>() ? 1 : 0;
    }
    return count;
}

}  // namespace

TEST(OrderInvariants, NewOrdersAreWellFormedForAnyLengthAndHorizon)
{
    std::mt19937 rng(20260923);
    for (int it = 0; it < kIterations; ++it)
    {
        std::uniform_int_distribution<std::size_t> length(1, 60);
        const auto route = random_route(rng, length(rng), 0, 0.0);
        std::uniform_int_distribution<std::size_t> horizon(0, route.size());
        const std::size_t released = horizon(rng);
        SCOPED_TRACE("route " + std::to_string(route.size()) + ", released " + std::to_string(released));

        const auto order = vda::build_route_order(1, "o", "M", "S", "base", {0, 0, 0}, route, "map", 0, released);
        expect_well_formed(order, 0);
        EXPECT_EQ(order.at("nodes").size(), route.size() + 1);
        EXPECT_EQ(released_nodes(order), released + 1);
        EXPECT_EQ(order.at("nodes").back().at("nodeId"), route.back().node_id);
    }
}

TEST(OrderInvariants, HorizonUpdatesStartAtTheLastReleasedNode)
{
    std::mt19937 rng(7);
    for (int it = 0; it < kIterations; ++it)
    {
        std::uniform_int_distribution<std::size_t> length(2, 40);
        const auto route = random_route(rng, length(rng), 0, 0.0);
        std::uniform_int_distribution<std::size_t> sent(1, route.size() - 1);
        const std::size_t before = sent(rng);
        std::uniform_int_distribution<std::size_t> more(before + 1, route.size());
        const std::size_t after = more(rng);

        const auto update = vda::build_route_order(2, "o", "M", "S", "base", {0, 0, 0}, route, "map", 1, after, before);
        expect_well_formed(update, 2 * static_cast<int>(before));
        EXPECT_EQ(update.at("nodes")[0].at("nodeId"), route[before - 1].node_id);
        EXPECT_EQ(released_nodes(update), after - before + 1);
    }
}

// A replanned route that repeats the released part is attached without touching the base.
TEST(OrderInvariants, StitchingKeepsTheBaseAndNeverShrinksTheHorizon)
{
    std::mt19937 rng(42);
    int stitched = 0;
    for (int it = 0; it < kIterations; ++it)
    {
        std::uniform_int_distribution<std::size_t> length(2, 30);
        const auto current = random_route(rng, length(rng), 0, 0.0);
        std::uniform_int_distribution<std::size_t> release(1, current.size());
        const std::size_t released = release(rng);
        std::uniform_int_distribution<std::size_t> pass(0, released - 1);
        const std::size_t traversed = pass(rng);

        std::vector<vda::RouteWaypoint> replanned(current.begin() + static_cast<std::ptrdiff_t>(traversed),
                                                  current.begin() + static_cast<std::ptrdiff_t>(released));
        std::uniform_int_distribution<std::size_t> tail(0, 10);
        const auto fresh = random_route(rng, tail(rng), 1000, current[released - 1].pose.x);
        replanned.insert(replanned.end(), fresh.begin(), fresh.end());
        SCOPED_TRACE("route " + std::to_string(current.size()) + ", released " + std::to_string(released) +
                     ", traversed " + std::to_string(traversed) + ", new " + std::to_string(replanned.size()));

        const auto plan = vda::plan_stitch(current, released, traversed, replanned);
        ASSERT_TRUE(plan.has_value());
        ++stitched;
        ASSERT_GE(plan->route.size(), released);
        for (std::size_t i = 0; i < released; ++i)
        {
            EXPECT_EQ(plan->route[i].node_id, current[i].node_id) << "base node " << i << " changed";
        }
        EXPECT_EQ(plan->stitch_index, released);
        EXPECT_LE(plan->consumed, released);

        std::uniform_int_distribution<std::size_t> want(0, replanned.size());
        const std::size_t new_released = vda::stitched_released_count(*plan, released, want(rng));
        EXPECT_GE(new_released, released) << "the released horizon shrank";
        EXPECT_LE(new_released, plan->route.size());

        if (!plan->unchanged)
        {
            const auto update = vda::build_route_order(3, "o", "M", "S", "base", {0, 0, 0}, plan->route, "map", 2,
                                                       new_released, plan->stitch_index);
            expect_well_formed(update, 2 * static_cast<int>(released));
            EXPECT_EQ(update.at("nodes")[0].at("nodeId"), current[released - 1].node_id);
        }
    }
    EXPECT_EQ(stitched, kIterations);
}

TEST(OrderInvariants, AReplanThatMovesAReleasedPointIsRefused)
{
    std::mt19937 rng(99);
    for (int it = 0; it < kIterations; ++it)
    {
        std::uniform_int_distribution<std::size_t> length(2, 30);
        const auto current = random_route(rng, length(rng), 0, 0.0);
        std::uniform_int_distribution<std::size_t> release(1, current.size());
        const std::size_t released = release(rng);
        std::uniform_int_distribution<std::size_t> pass(0, released - 1);
        const std::size_t traversed = pass(rng);

        std::vector<vda::RouteWaypoint> replanned(current.begin() + static_cast<std::ptrdiff_t>(traversed),
                                                  current.end());
        std::uniform_int_distribution<std::size_t> pick(0, released - traversed - 1);
        std::size_t moved = pick(rng);
        // Move a whole group of turns in place, from its first point.
        while (moved > 0 && current[traversed + moved - 1].node_id == current[traversed + moved].node_id)
        {
            --moved;
        }
        replanned[moved].node_id = "detour";
        replanned[moved].pose.y += 2.0;
        // Turns in place at the moved point go along with it.
        for (std::size_t k = moved + 1; k < replanned.size() && current[traversed + k].node_id == current[traversed + moved].node_id; ++k)
        {
            replanned[k] = replanned[moved];
        }
        SCOPED_TRACE("released " + std::to_string(released) + ", traversed " + std::to_string(traversed) +
                     ", moved " + std::to_string(moved));
        EXPECT_FALSE(vda::plan_stitch(current, released, traversed, replanned).has_value());
    }
}

TEST(OrderInvariants, TheSameRouteAgainIsUnchanged)
{
    std::mt19937 rng(3);
    for (int it = 0; it < kIterations; ++it)
    {
        std::uniform_int_distribution<std::size_t> length(2, 30);
        const auto current = random_route(rng, length(rng), 0, 0.0);
        std::uniform_int_distribution<std::size_t> release(1, current.size());
        const std::size_t released = release(rng);
        std::uniform_int_distribution<std::size_t> pass(0, released - 1);
        const std::size_t traversed = pass(rng);
        const std::vector<vda::RouteWaypoint> again(current.begin() + static_cast<std::ptrdiff_t>(traversed), current.end());

        const auto plan = vda::plan_stitch(current, released, traversed, again);
        ASSERT_TRUE(plan.has_value());
        EXPECT_TRUE(plan->unchanged);
        EXPECT_EQ(plan->route.size(), current.size());
        EXPECT_EQ(vda::stitched_released_count(*plan, released, again.size()), released);
    }
}
