#ifndef ROBOT_REGISTRATION_HPP
#define ROBOT_REGISTRATION_HPP

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "vda5050_fleet_adapter_full_control/vda5050/factsheet_handler.hpp"

namespace vda5050_fleet_adapter_full_control::core {

// A robot to add to a fleet: its broker identity, RMF charger and frame transform.
struct RobotSpec
{
    std::string name;
    std::string manufacturer;
    std::string serial;
    std::string charger;
    bool responsive_wait = false;
    double rotation = 0.0;
    double scale = 1.0;
    double tx = 0.0;
    double ty = 0.0;
    // Defined in the fleet config file, so it cannot be removed at runtime.
    bool from_config = false;
};

// A robot already registered in this fleet or another one.
struct KnownRobot
{
    std::string fleet;
    std::string name;
    std::string manufacturer;
    std::string serial;
    std::string charger;
    // Removed at runtime; its identity stays reserved until the adapter restarts.
    bool retired = false;
};

// Planning limits the fleet's RMF traits assume for every robot in it.
struct FleetLimits
{
    std::string fleet;
    double footprint_radius = 0.0;
    double linear_speed = 0.0;
    double linear_acceleration = 0.0;
    // Relative margin a robot's speed and acceleration may differ from these by.
    double tolerance = 0.0;
};

// What the broker showed about a robot that is not registered yet.
struct CandidateFacts
{
    bool seen = false;
    bool online = false;
    std::optional<vda5050::ParsedFactsheet> factsheet;
    bool has_state = false;
    bool pose_initialized = false;
    double x = 0.0;
    double y = 0.0;
    double theta = 0.0;
    std::string map_id;
};

// Questions about the fleet's navigation graph; the pose is in the RMF frame.
struct GraphFacts
{
    // Names of every charger waypoint.
    std::function<std::vector<std::string>()> chargers;
    std::function<bool(const std::string &waypoint)> is_charger;
    std::function<bool(const std::string &map)> has_map;
    std::function<bool(const std::string &map, double x, double y, double theta)> on_graph;
};

struct Finding
{
    std::string code;
    std::string message;
};

struct Verdict
{
    std::vector<Finding> errors;
    std::vector<Finding> warnings;
    // True when the only obstacle is that some checks could not be made.
    bool needs_confirmation = false;

    bool ok() const { return errors.empty(); }
    bool has_error(const std::string &code) const;
};

struct FleetView
{
    FleetLimits limits;
    // Every robot of this fleet and of the others.
    std::vector<KnownRobot> robots;
    // Factsheets of the robots already in this fleet.
    std::vector<vda5050::ParsedFactsheet> reference_factsheets;
};

// Checks that need nothing from the broker: name, identity, transform and charger.
Verdict validate_spec(const RobotSpec &spec, const FleetView &fleet, const GraphFacts &graph);

// Whether two specs describe the same robot: identity, name, charger, transform and waiting behaviour all match.
bool same_robot(const RobotSpec &a, const RobotSpec &b);

// validate_spec plus the broker's view of pose, type, speed and size; unverifiable checks are errors unless confirmed.
Verdict validate_new_robot(const RobotSpec &spec, const FleetView &fleet, const CandidateFacts &candidate, const GraphFacts &graph, bool confirm_unverified);

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // ROBOT_REGISTRATION_HPP
