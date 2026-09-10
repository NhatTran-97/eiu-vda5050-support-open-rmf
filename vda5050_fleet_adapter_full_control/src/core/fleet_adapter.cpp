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

#include "vda5050_fleet_adapter_full_control/core/config.hpp"
#include "vda5050_fleet_adapter_full_control/core/robot.hpp"
#include "vda5050_fleet_adapter_full_control/rmf/connector.hpp"

namespace vda5050_fleet_adapter_full_control::core {

namespace {
using rmf_fleet_adapter::agv::Adapter;
using rmf_fleet_adapter::agv::EasyFullControl;
}  // namespace

int run_fleet_adapter(int argc, char **argv)
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
        // ── EasyFullControl fleet from the rmf_fleet block of config + nav graph ──
        auto fleet_config = EasyFullControl::FleetConfiguration::from_config_files(
            args.config_file, args.nav_graph);
        if (!fleet_config)
        {
            RCLCPP_FATAL(logger, "Failed to parse fleet configuration from %s",
                         args.config_file.c_str());
            return 1;
        }

        // RMF still validates the initial RobotState battery value when building a
        // task assignment. When battery accounting is disabled for development,
        // report a healthy SoC to RMF until the VDA5050 client publishes trustworthy
        // battery telemetry. Otherwise batteryCharge=0 prevents every task bid.
        const bool account_for_battery_drain = fleet_config->account_for_battery_drain();
        if (!account_for_battery_drain)
        {
            RCLCPP_WARN(logger,
                        "Battery accounting is disabled; reporting battery SoC=1.0 to RMF");
        }

        auto fleet = adapter->add_easy_fleet(*fleet_config);

        // ── VDA5050 / MQTT settings (the `vda5050:` block, read by this adapter) ──
        const Config config(args.config_file);

        auto connector = std::make_shared<rmf::Connector>(
            logger, config.mqtt().broker_url, config.interface_name(),
            config.mqtt().username, config.mqtt().password);
        connector->start();

        std::map<std::string, std::shared_ptr<RobotAdapter>> robots;
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
            robots[name] = std::make_shared<RobotAdapter>(logger, name, *connector);
        }

        // ── Update loop: VDA5050 state -> RMF ────────────────────────────────────
        std::atomic<bool> running{true};
        const auto period = std::chrono::duration<double>(1.0 / config.update_rate_hz());

        std::thread update_thread([&] {
            while (running && rclcpp::ok())
            {
                for (auto &[name, robot] : robots)
                {
                    try
                    {
                        if (!connector->is_online(name))
                        {
                            if (robot->added())
                            {
                                RCLCPP_WARN_THROTTLE(
                                    logger, *adapter->node()->get_clock(), 10000,
                                    "Robot '%s' is offline - no recent VDA5050 state", name.c_str());
                            }
                            continue;
                        }

                        const auto data = connector->get_data(name);
                        if (!data)
                        {
                            continue;
                        }
                        const double rmf_battery_soc =
                            account_for_battery_drain ? data->battery_soc : 1.0;
                        EasyFullControl::RobotState state(
                            data->map_name,
                            Eigen::Vector3d(data->position[0], data->position[1], data->position[2]),
                            rmf_battery_soc);

                        if (!robot->added())
                        {
                            auto handle = fleet->add_robot(
                                name, state,
                                *fleet_config->get_known_robot_configuration(name),
                                robot->make_callbacks());
                            if (handle)
                            {
                                robot->set_update_handle(handle);
                                RCLCPP_INFO(logger, "Robot '%s' added to RMF fleet", name.c_str());
                            }
                        }
                        else
                        {
                            robot->update(state);
                        }
                    }
                    catch (const std::exception &e)
                    {
                        RCLCPP_ERROR(logger, "update_loop error for '%s': %s", name.c_str(), e.what());
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
