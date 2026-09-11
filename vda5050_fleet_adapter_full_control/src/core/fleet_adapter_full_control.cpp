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
#include <rmf_fleet_adapter/agv/Adapter.hpp>
#include <rmf_fleet_adapter/agv/EasyFullControl.hpp>
#include <rmf_traffic/agv/Planner.hpp>
#include <rmf_traffic_ros2/Time.hpp>

#include "vda5050_fleet_adapter_full_control/core/config.hpp"
#include "vda5050_fleet_adapter_full_control/core/operator_interface.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/robot_command_handle.hpp"

namespace vda5050_fleet_adapter_full_control::core {

namespace {
using rmf_fleet_adapter::agv::Adapter;
using rmf_fleet_adapter::agv::EasyFullControl;

// Maximum time to wait for an RMF robot-registration callback before retrying.
constexpr auto kRegistrationTimeout = std::chrono::seconds(30);
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
        // Reuse RMF's FleetConfiguration parser for fleet, graph, and task
        // planning parameters. Robot control is registered through FullControl.
        auto fleet_config = EasyFullControl::FleetConfiguration::from_config_files(
            args.config_file, args.nav_graph);
        if (!fleet_config)
        {
            RCLCPP_FATAL(logger, "Failed to parse fleet configuration from %s",
                         args.config_file.c_str());
            return 1;
        }

        const auto traits = fleet_config->vehicle_traits();
        const auto graph = fleet_config->graph();
        if (!traits || !graph)
        {
            RCLCPP_FATAL(logger, "Fleet configuration is missing vehicle traits or a graph");
            return 1;
        }

        auto fleet = adapter->add_fleet(fleet_config->fleet_name(), *traits, *graph,
                                        fleet_config->server_uri());
        if (!fleet)
        {
            RCLCPP_FATAL(logger, "add_fleet failed for '%s'",
                         fleet_config->fleet_name().c_str());
            return 1;
        }

        const bool account_for_battery_drain = fleet_config->account_for_battery_drain();
        if (!account_for_battery_drain)
        {
            RCLCPP_WARN(logger,
                        "Battery accounting is disabled; reporting battery SoC=1.0 to RMF");
        }

        // Apply the configured post-task finishing behavior.
        if (!fleet->set_task_planner_params(
                fleet_config->battery_system(), fleet_config->motion_sink(),
                fleet_config->ambient_sink(), fleet_config->tool_sink(),
                fleet_config->recharge_threshold(), fleet_config->recharge_soc(),
                account_for_battery_drain, fleet_config->finishing_request()))
        {
            RCLCPP_FATAL(logger, "set_task_planner_params failed -- this fleet would "
                                 "never bid for a task");
            return 1;
        }

        fleet->set_retreat_to_charger_interval(fleet_config->retreat_to_charger_interval());
        fleet->default_maximum_delay(fleet_config->max_delay());
        fleet->fleet_state_topic_publish_period(fleet_config->update_interval());

        // Register configured PerformAction categories with the task planner.
        for (const auto &[category, consider] : fleet_config->action_consideration())
        {
            fleet->add_performable_action(category, consider);
            RCLCPP_INFO(logger, "Fleet '%s' can perform action '%s'",
                        fleet_config->fleet_name().c_str(), category.c_str());
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
                RCLCPP_INFO(logger, "Fleet '%s' can perform delivery tasks",
                            fleet_config->fleet_name().c_str());
            }
            else if (task == "patrol")
            {
                fleet->consider_patrol_requests(consider);
                RCLCPP_INFO(logger, "Fleet '%s' can perform patrol tasks",
                            fleet_config->fleet_name().c_str());
            }
            else if (task == "clean")
            {
                fleet->consider_cleaning_requests(consider);
                RCLCPP_INFO(logger, "Fleet '%s' can perform cleaning tasks",
                            fleet_config->fleet_name().c_str());
            }
        }

        for (const auto &[lift, level] : fleet_config->lift_emergency_levels())
        {
            fleet->set_lift_emergency_level(lift, level);
        }

        // VDA5050 and MQTT settings
        const Config config(args.config_file);

        auto connector = std::make_shared<rmf::Connector>(
            logger, config.mqtt().broker_url, config.interface_name(),
            config.mqtt().username, config.mqtt().password);
        connector->start();

        const double nominal_speed = traits->linear().get_nominal_velocity();

        // Per-robot FullControl settings applied after RMF registration.
        struct RobotSetup
        {
            std::optional<std::size_t> charger_index;
            bool responsive_wait = false;
        };
        std::map<std::string, RobotSetup> robot_setup;

        std::map<std::string, std::shared_ptr<rmf::VdaRobotCommandHandle>> robots;
        std::set<std::pair<std::string, std::string>> seen_identities;
        for (const auto &name : fleet_config->known_robots())
        {
            const RobotConfig rc = config.robot_config(name);
            if (!seen_identities.insert({rc.manufacturer, rc.serial}).second)
            {
                throw std::runtime_error(
                    "robot '" + name + "': manufacturer/serial (" + rc.manufacturer + "/" +
                    rc.serial + ") is already used by another robot in this fleet -- "
                    "their VDA5050 MQTT state would be indistinguishable");
            }
            connector->add_robot(name, rc.manufacturer, rc.serial, rc.transform);
            robots[name] = std::make_shared<rmf::VdaRobotCommandHandle>(
                logger, name, *connector, graph, nominal_speed,
                adapter->node()->get_clock());

            RobotSetup setup;
            setup.responsive_wait = fleet_config->default_responsive_wait();
            if (const auto robot_cfg = fleet_config->get_known_robot_configuration(name))
            {
                if (robot_cfg->responsive_wait().has_value())
                {
                    setup.responsive_wait = *robot_cfg->responsive_wait();
                }
                if (!robot_cfg->compatible_chargers().empty())
                {
                    const auto &charger_name = robot_cfg->compatible_chargers().front();
                    if (const auto *charger_wp = graph->find_waypoint(charger_name))
                    {
                        setup.charger_index = charger_wp->index();
                    }
                    else
                    {
                        RCLCPP_ERROR(logger,
                                     "Robot '%s': charger waypoint '%s' not found in the nav "
                                     "graph -- it will use whatever charger RMF finds nearest",
                                     name.c_str(), charger_name.c_str());
                    }
                }
            }
            robot_setup[name] = setup;
        }

        // Register operator controls on the adapter node.
        std::map<std::string, RobotHooks> hooks;
        for (const auto &[name, command] : robots)
        {
            hooks[name] = RobotHooks{
                [command]() { return command->pause(); },
                [command]() { return command->resume(); }};
        }
        OperatorInterface operator_interface(*adapter->node(), *connector, std::move(hooks));

        // VDA5050 state -> RMF update loop
        std::atomic<bool> running{true};
        const auto period = std::chrono::duration<double>(1.0 / config.update_rate_hz());

        // Tracks asynchronous RMF robot registrations.
        std::map<std::string, std::chrono::steady_clock::time_point> registration_started;

        std::thread update_thread([&] {
            while (running && rclcpp::ok())
            {
                for (auto &[name, command] : robots)
                {
                    try
                    {
                        if (!connector->is_online(name))
                        {
                            if (command->added())
                            {
                                command->set_online(false);
                                RCLCPP_WARN_THROTTLE(
                                    logger, *adapter->node()->get_clock(), 10000,
                                    "Robot '%s' is offline - no recent VDA5050 state",
                                    name.c_str());
                            }
                            continue;
                        }
                        command->set_online(true);

                        const auto data = connector->get_data(name);
                        if (!data)
                        {
                            continue;
                        }

                        if (!command->added())
                        {
                            const auto pending = registration_started.find(name);
                            if (pending != registration_started.end())
                            {
                                if (std::chrono::steady_clock::now() - pending->second <
                                    kRegistrationTimeout)
                                {
                                    // Wait for the in-flight registration.
                                    continue;
                                }
                                RCLCPP_WARN(logger,
                                            "Robot '%s' registration did not complete within "
                                            "%lds -- retrying",
                                            name.c_str(),
                                            static_cast<long>(kRegistrationTimeout.count()));
                            }

                            const Eigen::Vector3d position(
                                data->position[0], data->position[1], data->position[2]);
                            auto starts = rmf_traffic::agv::compute_plan_starts(
                                *graph, data->map_name, position,
                                rmf_traffic_ros2::convert(adapter->node()->now()));
                            if (starts.empty())
                            {
                                RCLCPP_WARN_THROTTLE(
                                    logger, *adapter->node()->get_clock(), 10000,
                                    "Robot '%s' at (%.2f, %.2f) on '%s' does not merge "
                                    "onto the nav graph -- cannot add it to RMF yet",
                                    name.c_str(), position.x(), position.y(),
                                    data->map_name.c_str());
                                continue;
                            }

                            const RobotSetup setup = robot_setup.at(name);
                            auto handle_cb = [command, name, logger, setup](
                                std::shared_ptr<rmf_fleet_adapter::agv::RobotUpdateHandle> handle)
                            {
                                // Apply settings owned by the FullControl caller.
                                if (setup.charger_index.has_value())
                                {
                                    handle->set_charger_waypoint(*setup.charger_index);
                                }
                                handle->enable_responsive_wait(setup.responsive_wait);
                                command->set_update_handle(std::move(handle));
                                RCLCPP_INFO(logger, "Robot '%s' added to RMF fleet",
                                            name.c_str());
                            };

                            registration_started[name] = std::chrono::steady_clock::now();
                            fleet->add_robot(command, name, traits->profile(),
                                             std::move(starts), std::move(handle_cb));
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
                        RCLCPP_ERROR(logger, "update_loop error for '%s': %s", name.c_str(),
                                     e.what());
                    }
                }
                std::this_thread::sleep_for(
                    std::chrono::duration_cast<std::chrono::milliseconds>(period));
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
        return 1;
    }

    rclcpp::shutdown();
    RCLCPP_INFO(logger, "[vda5050_fleet_adapter_full_control] shutdown complete");
    std::fflush(nullptr);
    std::_Exit(0);
}

}  // namespace vda5050_fleet_adapter_full_control::core
