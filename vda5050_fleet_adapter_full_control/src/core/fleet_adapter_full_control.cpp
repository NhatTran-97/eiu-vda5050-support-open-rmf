#include "vda5050_fleet_adapter_full_control/core/fleet_adapter.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <Eigen/Geometry>

#include <rclcpp/rclcpp.hpp>
#include <rmf_fleet_adapter/StandardNames.hpp>
#include <rmf_fleet_adapter/agv/Adapter.hpp>
#include <rmf_fleet_adapter/agv/EasyFullControl.hpp>
#include <rmf_fleet_msgs/msg/lane_request.hpp>
#include <rmf_traffic/agv/Planner.hpp>
#include <rmf_traffic_ros2/Time.hpp>

#include "vda5050_fleet_adapter_full_control/core/config.hpp"
#include "vda5050_fleet_adapter_full_control/core/metrics_report.hpp"
#include "vda5050_fleet_adapter_full_control/core/operator_interface.hpp"
#include "vda5050_fleet_adapter_full_control/core/registration_interface.hpp"
#include "vda5050_fleet_adapter_full_control/core/robot_manager.hpp"
#include "vda5050_fleet_adapter_full_control/core/runtime_robots.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/robot_command_handle.hpp"
#include "vda5050_fleet_adapter_full_control/util/loop_pacer.hpp"
#include "vda5050_fleet_adapter_full_control/util/metrics.hpp"

namespace vda5050_fleet_adapter_full_control::core {

namespace {
using rmf_fleet_adapter::agv::Adapter;
using rmf_fleet_adapter::agv::EasyFullControl;

// Stop ROS and end the process with `code` without running destructors.
[[noreturn]] void exit_now(int code)
{
    rclcpp::shutdown();
    std::fflush(nullptr);
    std::_Exit(code);
}
}  // namespace

int run_fleet_adapter_full_control(int argc, char **argv)
{
    rclcpp::init(argc, argv);
    const Args args = parse_args(argc, argv);

    auto adapter = Adapter::make("vda5050_fleet_adapter_full_control");
    if (!adapter)
    {
        std::fprintf(stderr, "Failed to create RMF adapter (schedule node?)\n");
        return 1;
    }
    const auto logger = adapter->node()->get_logger();

    if (args.config_file.empty() || args.nav_graph.empty())
    {
        RCLCPP_FATAL(logger, "Required: -c <config.yaml> -n <nav_graph.yaml>");
        return 1;
    }

    adapter->start();

    try
    {
        // Load VDA5050 and MQTT settings before configuring the RMF fleet.
        const Config config(args.config_file);

        // Load RMF fleet, graph, and task settings for FullControl.
        auto fleet_config = EasyFullControl::FleetConfiguration::from_config_files(args.config_file, args.nav_graph, config.server_uri());
        if (!fleet_config)
        {
            RCLCPP_FATAL(logger, "Failed to parse fleet configuration from %s", args.config_file.c_str());
            exit_now(1);
        }
        if (config.server_uri())
        {
            RCLCPP_INFO(logger, "Broadcasting task/fleet updates to %s", config.server_uri()->c_str());
        }

        const auto traits = fleet_config->vehicle_traits();
        const auto graph = fleet_config->graph();
        if (!traits || !graph)
        {
            RCLCPP_FATAL(logger, "Fleet configuration is missing vehicle traits or a graph");
            exit_now(1);
        }

        auto fleet = adapter->add_fleet(fleet_config->fleet_name(), *traits, *graph,  fleet_config->server_uri());
        if (!fleet)
        {
            RCLCPP_FATAL(logger, "add_fleet failed for '%s'", fleet_config->fleet_name().c_str());
            exit_now(1);
        }

        const bool account_for_battery_drain = fleet_config->account_for_battery_drain();
        if (!account_for_battery_drain)
        {
            RCLCPP_WARN(logger,"Battery accounting is disabled; reporting battery SoC=1.0 to RMF");
        }

        // Apply the configured post-task finishing behavior.
        if (!fleet->set_task_planner_params(fleet_config->battery_system(), fleet_config->motion_sink(), fleet_config->ambient_sink(), fleet_config->tool_sink(),
                fleet_config->recharge_threshold(), fleet_config->recharge_soc(), account_for_battery_drain, fleet_config->finishing_request()))
        {
            RCLCPP_FATAL(logger, "set_task_planner_params failed -- this fleet would " "never bid for a task");
            exit_now(1);
        }

        fleet->set_retreat_to_charger_interval(fleet_config->retreat_to_charger_interval());
        fleet->default_maximum_delay(fleet_config->max_delay());
        fleet->fleet_state_topic_publish_period(fleet_config->update_interval());

        // Apply no-go zone / lane closures requested from the operator UI.
        const std::string this_fleet_name = fleet_config->fleet_name();
        const auto lanes_changed = std::make_shared<std::atomic<bool>>(false);
        const auto lane_request_sub = adapter->node()->create_subscription<rmf_fleet_msgs::msg::LaneRequest>(rmf_fleet_adapter::LaneClosureRequestTopicName, rclcpp::QoS(10).reliable().transient_local(),
            [fleet, this_fleet_name, lanes_changed](rmf_fleet_msgs::msg::LaneRequest::UniquePtr msg)
            {
                if (msg->fleet_name != this_fleet_name)
                {
                    return;
                }
                if (!msg->close_lanes.empty())
                {
                    fleet->close_lanes(std::vector<std::size_t>(msg->close_lanes.begin(), msg->close_lanes.end()));
                }
                if (!msg->open_lanes.empty())
                {
                    fleet->open_lanes(std::vector<std::size_t>(msg->open_lanes.begin(), msg->open_lanes.end()));
                }
                lanes_changed->store(true);
            });

        // Register configured PerformAction categories with the task planner.
        for (const auto &[category, consider] : fleet_config->action_consideration())
        {
            fleet->add_performable_action(category, consider);
            RCLCPP_INFO(logger, "Fleet '%s' can perform action '%s'", fleet_config->fleet_name().c_str(), category.c_str());
        }

        // Register configured task capabilities with their RMF request handlers.
        for (const auto &[task, consider] : fleet_config->task_consideration())
        {
            if (!consider)
            {
                continue;
            }
            if (task == "delivery")
            {
                fleet->consider_delivery_requests(consider, consider);
                RCLCPP_INFO(logger, "Fleet '%s' can perform delivery tasks", fleet_config->fleet_name().c_str());
            }
            else if (task == "patrol")
            {
                fleet->consider_patrol_requests(consider);
                RCLCPP_INFO(logger, "Fleet '%s' can perform patrol tasks", fleet_config->fleet_name().c_str());
            }
            else if (task == "clean")
            {
                fleet->consider_cleaning_requests(consider);
                RCLCPP_INFO(logger, "Fleet '%s' can perform cleaning tasks", fleet_config->fleet_name().c_str());
            }
        }

        for (const auto &[lift, level] : fleet_config->lift_emergency_levels())
        {
            fleet->set_lift_emergency_level(lift, level);
        }

        auto connector = std::make_shared<rmf::Connector>(logger, config.mqtt().broker_url, config.interface_name(), config.mqtt().username, config.mqtt().password, config.mqtt().options);
        connector->set_strict_validation(config.strict_validation());
        connector->set_stale_state_streak(config.stale_state_streak());
        connector->set_cancel_policy(config.cancel_policy());
        connector->set_link_policy(config.link_policy());
        connector->set_node_deviation(config.node_deviation());
        connector->start();

        const double nominal_speed = traits->linear().get_nominal_velocity();

        // Position reports to RMF merge onto waypoints and lanes as the fleet's RMF settings say.
        rmf::RoutePolicy route_policy = config.route_policy();
        route_policy.merge_waypoint_m = fleet_config->default_max_merge_waypoint_distance();
        route_policy.merge_lane_m = fleet_config->default_max_merge_lane_distance();

        RobotManager manager(logger, *connector, graph, adapter->node()->get_clock(),{nominal_speed, config.honor_waypoint_timing(), config.stitch_on_replan(), route_policy, config.action_policy()});

        // Robots declared in the fleet config file.
        std::set<std::pair<std::string, std::string>> seen_identities;
        for (const auto &name : fleet_config->known_robots())
        {
            const RobotConfig rc = config.robot_config(name);
            if (!seen_identities.insert({rc.manufacturer, rc.serial}).second)
            {
                throw std::runtime_error("robot '" + name + "': manufacturer/serial (" + rc.manufacturer + "/" +
                    rc.serial + ") is already used by another robot in this fleet -- " "their VDA5050 MQTT state would be indistinguishable");
            }

            RobotSpec spec;
            spec.name = name;
            spec.manufacturer = rc.manufacturer;
            spec.serial = rc.serial;
            spec.rotation = rc.transform.rotation();
            spec.scale = rc.transform.scale();
            spec.tx = rc.transform.tx();
            spec.ty = rc.transform.ty();
            spec.from_config = true;
            spec.responsive_wait = fleet_config->default_responsive_wait();
            if (const auto robot_cfg = fleet_config->get_known_robot_configuration(name))
            {
                if (robot_cfg->responsive_wait().has_value())
                {
                    spec.responsive_wait = *robot_cfg->responsive_wait();
                }
                if (!robot_cfg->compatible_chargers().empty())
                {
                    spec.charger = robot_cfg->compatible_chargers().front();
                }
            }
            manager.add(spec);
        }

        const std::string fleet_name = fleet_config->fleet_name();
        const RegistrationConfig &registration_config = config.registration();
        // Time RMF may take to complete a robot's registration before it is tried again.
        const std::chrono::duration<double> registration_timeout(registration_config.timeout_s);
        const FleetLimits limits{fleet_name, traits->profile().footprint()->get_characteristic_length(), traits->linear().get_nominal_velocity(), traits->linear().get_nominal_acceleration(), registration_config.limit_tolerance};

        GraphFacts graph_facts;
        graph_facts.chargers = [graph]()
        {
            std::vector<std::string> names;
            for (std::size_t i = 0; i < graph->num_waypoints(); ++i)
            {
                const auto &waypoint = graph->get_waypoint(i);
                if (waypoint.is_charger() && waypoint.name())
                {
                    names.push_back(*waypoint.name());
                }
            }
            return names;
        };
        graph_facts.is_charger = [graph](const std::string &waypoint)
        {
            const auto *found = graph->find_waypoint(waypoint);
            return found && found->is_charger();
        };
        graph_facts.has_map = [graph](const std::string &map)
        {
            for (std::size_t i = 0; i < graph->num_waypoints(); ++i)
            {
                if (graph->get_waypoint(i).get_map_name() == map)
                {
                    return true;
                }
            }
            return false;
        };
        graph_facts.on_graph = [graph, node = adapter->node()](const std::string &map, double x, double y, double theta)
        {
            const Eigen::Vector3d position(x, y, theta);
            return !rmf_traffic::agv::compute_plan_starts(*graph, map, position, rmf_traffic_ros2::convert(node->now())).empty();
        };

        // Robots added while the adapter ran earlier; an entry that no longer passes the checks is skipped.
        const std::string runtime_path = runtime_robots_path(args.config_file, registration_config.runtime_robots_file);
        const auto runtime = load_runtime_robots(runtime_path);
        for (const auto &problem : runtime.problems)
        {
            RCLCPP_ERROR(logger, "Runtime robots: %s", problem.c_str());
        }
        for (const auto &spec : runtime.robots)
        {
            FleetView view;
            view.limits = limits;
            for (const auto &entry : manager.snapshot())
            {
                view.robots.push_back({fleet_name, entry->spec.name, entry->spec.manufacturer, entry->spec.serial, entry->spec.charger, false});
            }
            const Verdict verdict = validate_spec(spec, view, graph_facts);
            if (!verdict.ok())
            {
                for (const auto &error : verdict.errors)
                {
                    RCLCPP_ERROR(logger, "Runtime robot '%s' skipped: %s", spec.name.c_str(), error.message.c_str());
                }
                continue;
            }
            manager.add(spec);
            RCLCPP_INFO(logger, "Runtime robot '%s' (%s/%s) loaded from %s", spec.name.c_str(), spec.manufacturer.c_str(), spec.serial.c_str(), runtime_path.c_str());
        }

        // Register operator controls on the adapter node.
        std::map<std::string, RobotHooks> hooks;
        for (const auto &entry : manager.snapshot())
        {
            const auto command = entry->command;
            hooks[entry->spec.name] = RobotHooks{[command]() { return command->pause(); }, [command]() { return command->resume(); }};
        }
        // Nearest graph waypoint within waypoint_reached_m, named as in orders.
        const double reached_m = route_policy.waypoint_reached_m;
        const auto node_at = [graph, reached_m](const std::string &map, double x, double y)
        {
            std::optional<std::size_t> nearest;
            double best = reached_m;
            for (std::size_t i = 0; i < graph->num_waypoints(); ++i)
            {
                const auto &waypoint = graph->get_waypoint(i);
                const double distance = (waypoint.get_location() - Eigen::Vector2d(x, y)).norm();
                if (waypoint.get_map_name() == map && distance <= best)
                {
                    best = distance;
                    nearest = i;
                }
            }
            if (!nearest)
            {
                return std::string{};
            }
            const auto &waypoint = graph->get_waypoint(*nearest);
            return rmf::VdaRobotCommandHandle::derive_node_id(waypoint.name() ? *waypoint.name() : std::string{}, *nearest, waypoint.get_location().x(), waypoint.get_location().y());
        };
        OperatorInterface operator_interface(*adapter->node(), *connector, std::move(hooks), std::chrono::duration<double>(config.init_position_timeout_s()), node_at);

        RegistrationInterface registration(*adapter->node(), *connector, manager, operator_interface,
                                           {fleet_name, config.interface_name(), runtime_path, limits, graph_facts,
                                            fleet_config->default_responsive_wait(), registration_config.discovery_grace_s, registration_config.discovery_period_s});
        registration.publish_registry();

        // Update RMF from VDA5050 state.
        std::atomic<bool> running{true};
        const auto period = std::chrono::duration<double>(1.0 / config.update_rate_hz());
        util::LoopPacer pacer(std::chrono::steady_clock::now(), std::chrono::duration_cast<std::chrono::steady_clock::duration>(period));

        // Update pass times and overruns, reported with the message path's metrics.
        util::Histogram loop_pass;
        util::Counter loop_overruns;
        MetricsReporter metrics_reporter( *adapter->node(), std::chrono::duration<double>(config.metrics_period_s()), [&loop_pass, &loop_overruns, connector, period, fleet_name]()
            {
                nlohmann::json report = connector->metrics();
                report["fleet"] = fleet_name;
                report["update_loop"] = {{"pass_us", util::to_json(loop_pass.take())},
                                         {"overruns", loop_overruns.value()},
                                         {"period_ms", std::chrono::duration<double, std::milli>(period).count()}};
                return report;
            });

        std::thread update_thread([&] {
            while (running && rclcpp::ok())
            {
                const auto pass_started = std::chrono::steady_clock::now();
                const bool lanes_changed_now = lanes_changed->exchange(false);
                for (const auto &entry : manager.snapshot())
                {
                    if (entry->retired)
                    {
                        continue;
                    }
                    if (lanes_changed_now)
                    {
                        entry->command->report_position_again();
                    }
                    const std::string &name = entry->spec.name;
                    const auto &command = entry->command;
                    try
                    {
                        if (!connector->is_online(name))
                        {
                            if (command->added())
                            {
                                command->set_online(false);
                                RCLCPP_WARN_THROTTLE(logger, *adapter->node()->get_clock(), 10000, "Robot '%s' is offline - no recent VDA5050 state",name.c_str());
                            }
                            continue;
                        }
                        command->set_online(true);
                        connector->poll(name);

                        const auto data = connector->get_data(name);
                        if (!data)
                        {
                            // Decommission robots that have lost a usable pose.
                            if (command->added())
                            {
                                command->set_ready_for_orders(false, "no valid pose");
                                command->expire_traffic_hold();
                            }
                            else
                            {
                                RCLCPP_WARN_THROTTLE(logger, *adapter->node()->get_clock(), 10000,
                                    "Robot '%s' is online but its state has no usable pose (agvPosition missing or " "positionInitialized false) -- not added to RMF yet",name.c_str());
                            }
                            continue;
                        }

                        if (!command->added())
                        {
                            if (entry->registration_started.has_value())
                            {
                                if (std::chrono::steady_clock::now() - *entry->registration_started < registration_timeout)
                                {
                                    // Wait for the in-flight registration.
                                    continue;
                                }
                                RCLCPP_WARN(logger,"Robot '%s' registration did not complete within " "%.0fs -- retrying", name.c_str(), registration_timeout.count());
                            }

                            const Eigen::Vector3d position(data->position[0], data->position[1], data->position[2]);
                            auto starts = rmf_traffic::agv::compute_plan_starts(*graph, data->map_name, position, rmf_traffic_ros2::convert(adapter->node()->now()));
                            if (starts.empty())
                            {
                                RCLCPP_WARN_THROTTLE(logger, *adapter->node()->get_clock(), 10000, "Robot '%s' at (%.2f, %.2f) on '%s' does not merge " "onto the nav graph -- cannot add it to RMF yet",
                                    name.c_str(), position.x(), position.y(), data->map_name.c_str());
                                continue;
                            }

                            const auto charger_index = entry->charger_index;
                            const bool responsive_wait = entry->spec.responsive_wait;
                            const auto generation = entry->registration_generation;
                            const int this_attempt = ++(*generation);
                            auto handle_cb = [command, name, logger, charger_index, responsive_wait, generation, this_attempt](std::shared_ptr<rmf_fleet_adapter::agv::RobotUpdateHandle> handle)
                            {
                                if (generation->load() != this_attempt)
                                {
                                    // Ignore registration callbacks superseded by a retry.
                                    RCLCPP_WARN(logger,"Robot '%s' registration callback arrived " "after a retry superseded it -- ignoring", name.c_str());
                                    return;
                                }
                                // Apply settings owned by the FullControl caller.
                                if (charger_index.has_value())
                                {
                                    handle->set_charger_waypoint(*charger_index);
                                }
                                handle->enable_responsive_wait(responsive_wait);
                                command->set_update_handle(handle);
                                RCLCPP_INFO(logger, "Robot '%s' added to RMF fleet", name.c_str());
                            };

                            entry->registration_started = std::chrono::steady_clock::now();
                            fleet->add_robot(command, name, traits->profile(), std::move(starts), std::move(handle_cb));
                            continue;
                        }

                        rmf::RobotData reported = *data;
                        if (!account_for_battery_drain)
                        {
                            reported.battery_soc = 1.0;
                        }
                        command->update(reported);
                    }
                    catch (const std::exception &e)
                    {
                        RCLCPP_ERROR(logger, "update_loop error for '%s': %s", name.c_str(), e.what());
                    }
                }
                const auto overruns_before = pacer.overruns();
                const auto pass_ended = std::chrono::steady_clock::now();
                const auto wake = pacer.next(pass_ended);
                loop_pass.record(pass_ended - pass_started);
                if (pacer.overruns() > overruns_before)
                {
                    loop_overruns.add(pacer.overruns() - overruns_before);
                    RCLCPP_WARN_THROTTLE(logger, *adapter->node()->get_clock(), 10000,  "Update loop pass took %.0f ms, over the %.0f ms period (%zu overrun(s) so far)",
                        std::chrono::duration<double, std::milli>(pass_ended - pass_started).count(),std::chrono::duration<double, std::milli>(period).count(), pacer.overruns());
                }
                std::this_thread::sleep_until(wake);
            }
        });

        adapter->wait();
        running = false;
        if (update_thread.joinable())
        {
            update_thread.join();
        }

        connector->shutdown();
    }
    catch (const std::exception &e)
    {
        RCLCPP_FATAL(logger, "Fleet adapter startup failed: %s", e.what());
        exit_now(1);
    }

    rclcpp::shutdown();
    RCLCPP_INFO(logger, "[vda5050_fleet_adapter_full_control] shutdown complete");
    std::fflush(nullptr);
    std::_Exit(0);
}

}  // namespace vda5050_fleet_adapter_full_control::core
