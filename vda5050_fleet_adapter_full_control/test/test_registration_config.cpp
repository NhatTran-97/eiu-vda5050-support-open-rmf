#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "vda5050_fleet_adapter_full_control/core/config.hpp"
#include "vda5050_fleet_adapter_full_control/core/runtime_robots.hpp"

using namespace vda5050_fleet_adapter_full_control::core;

namespace {

// Write a config file whose vda5050 section ends with `registration` and return its path.
std::string write_config(const std::string &registration)
{
    const std::string path = ::testing::TempDir() + "registration_config_test.yaml";
    std::ofstream(path) << "vda5050:\n  interface_name: test\n  mqtt:\n    host: localhost\n    port: 1883\n"
                        << registration;
    return path;
}

}  // namespace

TEST(RegistrationConfigTest, DefaultsApplyWhenTheBlockIsAbsent)
{
    const Config config(write_config(""));
    const RegistrationConfig defaults;
    EXPECT_DOUBLE_EQ(config.registration().discovery_grace_s, defaults.discovery_grace_s);
    EXPECT_DOUBLE_EQ(config.registration().discovery_period_s, defaults.discovery_period_s);
    EXPECT_DOUBLE_EQ(config.registration().limit_tolerance, defaults.limit_tolerance);
    EXPECT_TRUE(config.registration().runtime_robots_file.empty());
}

TEST(RegistrationConfigTest, EveryKeyIsRead)
{
    const Config config(write_config(
        "  registration:\n"
        "    discovery_grace_s: 3.5\n"
        "    discovery_period_s: 0.5\n"
        "    limit_tolerance: 0.1\n"
        "    runtime_robots_file: extra/robots.yaml\n"));
    EXPECT_DOUBLE_EQ(config.registration().discovery_grace_s, 3.5);
    EXPECT_DOUBLE_EQ(config.registration().discovery_period_s, 0.5);
    EXPECT_DOUBLE_EQ(config.registration().limit_tolerance, 0.1);
    EXPECT_EQ(config.registration().runtime_robots_file, "extra/robots.yaml");
}

TEST(RegistrationConfigTest, ARobotMissingFromTheConfigIsNamedInTheError)
{
    const Config config(write_config(""));
    try
    {
        config.robot_config("ghost");
        FAIL() << "expected an error";
    }
    catch (const std::runtime_error &e)
    {
        EXPECT_NE(std::string(e.what()).find("ghost"), std::string::npos) << e.what();
    }
}

TEST(RegistrationConfigTest, AnEmptyBlockKeepsTheDefaults)
{
    const Config config(write_config("  registration:\n"));
    EXPECT_DOUBLE_EQ(config.registration().limit_tolerance, RegistrationConfig().limit_tolerance);
}

TEST(RegistrationConfigTest, ValuesOutOfRangeAreRejectedAtStartup)
{
    const std::vector<std::string> bad = {
        "    discovery_grace_s: -1\n",  "    discovery_grace_s: .nan\n", "    discovery_grace_s: 100000\n",
        "    discovery_period_s: 0\n",  "    discovery_period_s: .inf\n", "    limit_tolerance: -0.1\n",
        "    limit_tolerance: 0.9\n",     "    limit_tolerance: high\n",
    };
    const auto rejected_for_registration = [](const std::string &text)
    {
        try
        {
            Config config(write_config(text));
        }
        catch (const std::runtime_error &e)
        {
            return std::string(e.what()).find("vda5050.registration") != std::string::npos;
        }
        return false;
    };
    for (const auto &line : bad)
    {
        EXPECT_TRUE(rejected_for_registration("  registration:\n" + line)) << line;
    }
    EXPECT_TRUE(rejected_for_registration("  registration: 5\n"));
}

TEST(RegistrationConfigTest, RuntimeFileDefaultsNextToTheConfigAndCanBeMoved)
{
    EXPECT_EQ(runtime_robots_path("/etc/fleet/config_tb3.yaml"), "/etc/fleet/config_tb3.runtime_robots.yaml");
    EXPECT_EQ(runtime_robots_path("/etc/fleet/config_tb3.yaml", "state/robots.yaml"), "/etc/fleet/state/robots.yaml");
    EXPECT_EQ(runtime_robots_path("/etc/fleet/config_tb3.yaml", "/var/lib/fleet/robots.yaml"),
              "/var/lib/fleet/robots.yaml");
}
