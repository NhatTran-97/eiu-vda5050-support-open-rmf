#include "vda5050_fleet_adapter_full_control/core/runtime_robots.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace vda5050_fleet_adapter_full_control::core {

namespace {

// Copies the owner (and optionally the mode) of a reference file, so a root process leaves the user's files editable.
void copy_ownership(const std::string &target, const std::string &reference, bool copy_mode)
{
    struct stat st;
    if (::stat(reference.c_str(), &st) != 0)
    {
        return;
    }
    if (copy_mode && ::chmod(target.c_str(), st.st_mode & 0777) != 0)
    {
        return;
    }
    // Only a privileged process may hand a file to another owner.
    if (::chown(target.c_str(), st.st_uid, st.st_gid) != 0)
    {
        return;
    }
}

}  // namespace

std::string runtime_robots_path(const std::string &config_file, const std::string &override_file)
{
    const std::filesystem::path config(config_file);
    if (!override_file.empty())
    {
        return (config.parent_path() / override_file).string();
    }
    return (config.parent_path() / (config.stem().string() + ".runtime_robots.yaml")).string();
}

RuntimeRobots load_runtime_robots(const std::string &path)
{
    RuntimeRobots result;
    if (!std::filesystem::exists(path))
    {
        return result;
    }

    YAML::Node root;
    try
    {
        root = YAML::LoadFile(path);
    }
    catch (const std::exception &e)
    {
        result.problems.push_back(path + " is not valid YAML: " + e.what());
        return result;
    }

    const YAML::Node robots = root["robots"];
    if (!robots || robots.IsNull())
    {
        return result;
    }
    if (!robots.IsMap())
    {
        result.problems.push_back(path + ": 'robots' must be a map of robot name to settings");
        return result;
    }

    for (const auto &entry : robots)
    {
        RobotSpec spec;
        try
        {
            spec.name = entry.first.as<std::string>();
            const YAML::Node &rc = entry.second;
            if (!rc.IsMap())
            {
                throw std::runtime_error("expected a map of settings");
            }
            if (!rc["manufacturer"] || !rc["serial"] || !rc["charger"])
            {
                throw std::runtime_error("manufacturer, serial and charger are required");
            }
            spec.manufacturer = rc["manufacturer"].as<std::string>();
            spec.serial = rc["serial"].as<std::string>();
            spec.charger = rc["charger"].as<std::string>();
            spec.responsive_wait = rc["responsive_wait"] ? rc["responsive_wait"].as<bool>() : false;
            if (rc["transform"])
            {
                const YAML::Node &t = rc["transform"];
                spec.rotation = t["rotation"] ? t["rotation"].as<double>() : 0.0;
                spec.scale = t["scale"] ? t["scale"].as<double>() : 1.0;
                if (t["translation"] && t["translation"].size() >= 2)
                {
                    spec.tx = t["translation"][0].as<double>();
                    spec.ty = t["translation"][1].as<double>();
                }
            }
        }
        catch (const std::exception &e)
        {
            result.problems.push_back("robot '" + spec.name + "' in " + path + ": " + e.what());
            continue;
        }
        result.robots.push_back(std::move(spec));
    }
    return result;
}

bool backup_runtime_robots(const std::string &path, std::string *error)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return true;
    }
    std::filesystem::copy_file(path, path + ".bak", std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec)
    {
        copy_ownership(path + ".bak", path, true);
    }
    if (ec)
    {
        if (error)
        {
            *error = "cannot back up " + path + ": " + ec.message();
        }
        return false;
    }
    return true;
}

bool save_runtime_robots(const std::string &path, const std::vector<RobotSpec> &robots, std::string *error)
{
    YAML::Emitter out;
    out << YAML::BeginMap << YAML::Key << "robots" << YAML::Value << YAML::BeginMap;
    for (const auto &r : robots)
    {
        out << YAML::Key << r.name << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "manufacturer" << YAML::Value << YAML::DoubleQuoted << r.manufacturer;
        out << YAML::Key << "serial" << YAML::Value << YAML::DoubleQuoted << r.serial;
        out << YAML::Key << "charger" << YAML::Value << r.charger;
        out << YAML::Key << "responsive_wait" << YAML::Value << r.responsive_wait;
        out << YAML::Key << "transform" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "rotation" << YAML::Value << r.rotation;
        out << YAML::Key << "scale" << YAML::Value << r.scale;
        out << YAML::Key << "translation" << YAML::Value << YAML::Flow << YAML::BeginSeq << r.tx << r.ty << YAML::EndSeq;
        out << YAML::EndMap << YAML::EndMap;
    }
    out << YAML::EndMap << YAML::EndMap;

    const std::string temporary = path + ".tmp";
    std::error_code ec;
    {
        std::ofstream file(temporary, std::ios::trunc);
        file << "# Robots added while the adapter was running. Managed by the adapter; delete a\n"
                "# robot's entry or the whole file to forget it.\n"
             << out.c_str() << "\n";
        if (!file)
        {
            if (error)
            {
                *error = "cannot write " + temporary;
            }
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }

    const bool existed = std::filesystem::exists(path, ec);
    const std::string folder = std::filesystem::path(path).parent_path().string();
    copy_ownership(temporary, existed ? path : (folder.empty() ? "." : folder), existed);

    std::filesystem::rename(temporary, path, ec);
    if (ec)
    {
        if (error)
        {
            *error = "cannot replace " + path + ": " + ec.message();
        }
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

}  // namespace vda5050_fleet_adapter_full_control::core
