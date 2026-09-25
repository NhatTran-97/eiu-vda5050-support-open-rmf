#include "vda5050_fleet_adapter_full_control/core/robot_registration.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "vda5050_fleet_adapter_full_control/rmf/transform.hpp"

namespace vda5050_fleet_adapter_full_control::core {

namespace {

constexpr std::size_t kMaxNameLength = 64;

bool is_alnum(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

// 1-64 letters, digits, '_' or '-', starting with a letter or digit.
bool valid_name(const std::string &name)
{
    if (name.empty() || name.size() > kMaxNameLength || !is_alnum(name.front()))
    {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) { return is_alnum(c) || c == '_' || c == '-'; });
}

// 1-64 characters that are safe as one MQTT topic level.
bool valid_identity_part(const std::string &text)
{
    if (text.empty() || text.size() > kMaxNameLength)
    {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char c) { return is_alnum(c) || c == '_' || c == '-' || c == '.'; });
}

std::string number(double value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    return buf;
}

void add(std::vector<Finding> &list, const std::string &code, const std::string &message)
{
    list.push_back({code, message});
}

bool close(double a, double b)
{
    return std::fabs(a - b) <= 1e-9;
}

}  // namespace

bool same_robot(const RobotSpec &a, const RobotSpec &b)
{
    return a.name == b.name && a.manufacturer == b.manufacturer && a.serial == b.serial && a.charger == b.charger &&
           a.responsive_wait == b.responsive_wait && close(a.rotation, b.rotation) && close(a.scale, b.scale) &&
           close(a.tx, b.tx) && close(a.ty, b.ty);
}

bool Verdict::has_error(const std::string &code) const
{
    return std::any_of(errors.begin(), errors.end(), [&](const Finding &f) { return f.code == code; });
}

Verdict validate_spec(const RobotSpec &spec, const FleetView &fleet, const GraphFacts &graph)
{
    Verdict verdict;

    if (!valid_name(spec.name))
    {
        add(verdict.errors, "name_invalid", "Name '" + spec.name + "' is not valid: use 1-64 letters, digits, '_' or '-', starting with a letter or digit.");
    }
    else
    {
        for (const auto &known : fleet.robots)
        {
            if (known.name == spec.name)
            {
                add(verdict.errors, "name_taken",  "A robot named '" + spec.name + "' already exists in fleet '" + known.fleet + "'" +
                        (known.retired ? " (removed in this session; register it again with the same settings to restore it, or restart the adapter to change them)." : "."));
                break;
            }
        }
    }

    if (!valid_identity_part(spec.manufacturer) || !valid_identity_part(spec.serial))
    {
        add(verdict.errors, "identity_invalid","Manufacturer and serial must be 1-64 characters from letters, digits, '.', '_' and '-'.");
    }
    else
    {
        for (const auto &known : fleet.robots)
        {
            if (known.manufacturer == spec.manufacturer && known.serial == spec.serial)
            {
                add(verdict.errors, "identity_taken",spec.manufacturer + "/" + spec.serial + " is already used by robot '" + known.name + "' in fleet '" + known.fleet + "'" +
                        (known.retired ? " (removed in this session; register it again with the same settings to restore it, or restart the adapter to change them)." : "."));
                break;
            }
        }
    }

    if (!std::isfinite(spec.rotation) || !std::isfinite(spec.scale) || spec.scale == 0.0 || !std::isfinite(spec.tx) || !std::isfinite(spec.ty))
    {
        add(verdict.errors, "transform_invalid", "Transform must have finite values and a non-zero scale.");
    }

    if (spec.charger.empty())
    {
        add(verdict.errors, "charger_missing", "A charger waypoint is required.");
    }
    else if (!graph.is_charger || !graph.is_charger(spec.charger))
    {
        add(verdict.errors, "charger_unknown", "'" + spec.charger + "' is not a charger waypoint of the nav graph.");
    }
    else
    {
        for (const auto &known : fleet.robots)
        {
            if (known.charger != spec.charger)
            {
                continue;
            }
            if (known.fleet == fleet.limits.fleet)
            {
                add(verdict.errors, "charger_taken", "Charger '" + spec.charger + "' is already used by robot '" + known.name + "' in this fleet" +
                        (known.retired ? " (removed in this session; RMF keeps its last position until the adapter restarts, so only that robot can be restored on it)." : "; each robot needs its own."));
                break;
            }
            add(verdict.warnings, "charger_shared", "Charger '" + spec.charger + "' is also used by robot '" + known.name + "' in fleet '" + known.fleet + "'.");
        }
    }

    return verdict;
}

Verdict validate_new_robot(const RobotSpec &spec, const FleetView &fleet, const CandidateFacts &candidate,
                           const GraphFacts &graph, bool confirm_unverified)
{
    Verdict verdict = validate_spec(spec, fleet, graph);
    // The broker facts are looked up by identity, so they mean nothing for a taken or malformed one.
    if (verdict.has_error("identity_invalid") || verdict.has_error("identity_taken"))
    {
        return verdict;
    }
    std::vector<std::string> unverified;

    // Pose: the robot must stand where RMF can register it.
    if (!candidate.seen)
    {
        unverified.push_back("the robot has not been seen on the broker");
    }
    else if (!candidate.online)
    {
        unverified.push_back("the robot is not online, so its pose cannot be checked");
    }
    else if (!candidate.has_state)
    {
        unverified.push_back("the robot has not reported its pose yet");
    }
    else if (!candidate.pose_initialized)
    {
        unverified.push_back("the robot is not localized, so its place on the nav graph cannot be checked; localize it " "from the dashboard once it is added");
    }
    else if (graph.has_map && !graph.has_map(candidate.map_id))
    {
        add(verdict.errors, "map_unknown", "The robot is on map '" + candidate.map_id + "', which this fleet's nav graph does not have.");
    }
    else if (graph.on_graph && !verdict.has_error("transform_invalid"))
    {
        const rmf::Transform transform(spec.rotation, spec.scale, spec.tx, spec.ty);
        const auto rmf_pose = transform.to_rmf(candidate.x, candidate.y, candidate.theta);
        if (!graph.on_graph(candidate.map_id, rmf_pose[0], rmf_pose[1], rmf_pose[2]))
        {
            add(verdict.errors, "off_graph", "The robot at (" + number(rmf_pose[0]) + ", " + number(rmf_pose[1]) + ") on '" + candidate.map_id +
                    "' is not on the nav graph, so RMF could not register it. Move it onto a lane or " "waypoint of this fleet's graph.");
        }
    }

    // Type, speed and size come from the factsheet.
    if (!candidate.factsheet.has_value() || !candidate.factsheet->has_content())
    {
        unverified.push_back("the robot published no factsheet, so its type, speed and size cannot be checked");
    }
    else
    {
        const auto &fs = *candidate.factsheet;
        const auto &limits = fleet.limits;

        if (fs.speed_max.has_value())
        {
            const double speed = *fs.speed_max;
            if (speed < limits.linear_speed * (1.0 - limits.tolerance))
            {
                add(verdict.errors, "speed_too_low", "The robot's maximum speed (" + number(speed) + " m/s) is below the fleet's planning speed (" +
                        number(limits.linear_speed) + " m/s); RMF would expect it to arrive earlier than it can.");
            }
            else if (speed > limits.linear_speed * (1.0 + limits.tolerance))
            {
                add(verdict.warnings, "speed_higher","The robot's maximum speed (" + number(speed) + " m/s) is above the fleet's planning speed (" +
                        number(limits.linear_speed) + " m/s); it may arrive earlier than RMF plans.");
            }
        }
        else
        {
            unverified.push_back("its maximum speed is not declared");
        }

        if (fs.acceleration_max.has_value() && *fs.acceleration_max < limits.linear_acceleration * (1.0 - limits.tolerance))
        {
            add(verdict.warnings, "accel_low", "The robot's maximum acceleration (" + number(*fs.acceleration_max) +  " m/s^2) is below the fleet's planning value (" + number(limits.linear_acceleration) + " m/s^2).");
        }

        if (fs.length.has_value() && fs.width.has_value())
        {
            const double radius = 0.5 * std::hypot(*fs.length, *fs.width);
            if (radius > limits.footprint_radius)
            {
                add(verdict.errors, "too_large", "The robot's size (" + number(*fs.length) + " x " + number(*fs.width) + " m, radius " +
                        number(radius) + " m) exceeds the fleet's footprint radius (" + number(limits.footprint_radius) + " m).");
            }
        }
        else
        {
            unverified.push_back("its size is not declared");
        }

        if (fs.series_name.empty())
        {
            unverified.push_back("it declares no series name, so its type cannot be compared");
        }
        else
        {
            const vda5050::ParsedFactsheet *reference = nullptr;
            for (const auto &ref : fleet.reference_factsheets)
            {
                if (!ref.series_name.empty())
                {
                    reference = &ref;
                    break;
                }
            }
            if (!reference)
            {
                unverified.push_back("no robot of this fleet has a factsheet to compare its type with");
            }
            else
            {
                const bool series_differs = fs.series_name != reference->series_name;
                const bool kinematic_differs = !fs.agv_kinematic.empty() && !reference->agv_kinematic.empty() && fs.agv_kinematic != reference->agv_kinematic;
                const bool class_differs = !fs.agv_class.empty() && !reference->agv_class.empty() && fs.agv_class != reference->agv_class;
                if (series_differs || kinematic_differs || class_differs)
                {
                    add(verdict.errors, "type_mismatch", "The robot is a '" + fs.series_name + "' (" + fs.agv_kinematic + "/" + fs.agv_class +") but this fleet's robots are '" + reference->series_name + "' (" +
                            reference->agv_kinematic + "/" + reference->agv_class +"); add it to a fleet of its own type.");
                }
            }
        }
    }

    if (!unverified.empty())
    {
        std::string list;
        for (const auto &item : unverified)
        {
            list += (list.empty() ? "" : "; ") + item;
        }
        if (confirm_unverified)
        {
            for (const auto &item : unverified)
            {
                add(verdict.warnings, "unverified", "Not verified: " + item + ".");
            }
        }
        else
        {
            add(verdict.errors, "unverified", "Some checks could not be made: " + list + ". Confirm to add the robot anyway.");
            verdict.needs_confirmation = verdict.errors.size() == 1;
        }
    }

    return verdict;
}

}  // namespace vda5050_fleet_adapter_full_control::core
