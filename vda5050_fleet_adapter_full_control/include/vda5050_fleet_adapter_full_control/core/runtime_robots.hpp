#ifndef RUNTIME_ROBOTS_HPP
#define RUNTIME_ROBOTS_HPP

#include <string>
#include <vector>

#include "vda5050_fleet_adapter_full_control/core/robot_registration.hpp"

namespace vda5050_fleet_adapter_full_control::core {

// Path of the runtime robots file: "<dir>/<stem>.runtime_robots.yaml" beside the config unless overridden.
std::string runtime_robots_path(const std::string &config_file, const std::string &override_file = {});

struct RuntimeRobots
{
    std::vector<RobotSpec> robots;
    // Entries or files that could not be read; each is skipped.
    std::vector<std::string> problems;
};

// Read the runtime robots file; a missing file is not a problem.
RuntimeRobots load_runtime_robots(const std::string &path);

// Copies the current file to "<path>.bak" before a rewrite; a missing file needs no backup.
bool backup_runtime_robots(const std::string &path, std::string *error);

// Write the runtime robots file through a temporary file so a crash cannot leave it half written.
bool save_runtime_robots(const std::string &path, const std::vector<RobotSpec> &robots, std::string *error);

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // RUNTIME_ROBOTS_HPP
