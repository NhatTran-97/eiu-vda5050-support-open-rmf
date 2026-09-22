#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "vda5050_fleet_adapter_full_control/core/runtime_robots.hpp"

using namespace vda5050_fleet_adapter_full_control::core;

namespace {

class RuntimeRobotsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        dir = std::filesystem::temp_directory_path() / ("runtime_robots_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        path = (dir / "config_x.runtime_robots.yaml").string();
    }
    void TearDown() override { std::filesystem::remove_all(dir); }

    void write(const std::string &text) { std::ofstream(path) << text; }

    std::filesystem::path dir;
    std::string path;
};

RobotSpec robot(const std::string &name, const std::string &serial)
{
    RobotSpec r;
    r.name = name;
    r.manufacturer = "ROBOTIS";
    r.serial = serial;
    r.charger = "charger_1";
    return r;
}

}  // namespace

TEST(RuntimeRobotsPath, SitsNextToTheConfigFile)
{
    EXPECT_EQ(runtime_robots_path("/etc/fleet/config_tb3.yaml"), "/etc/fleet/config_tb3.runtime_robots.yaml");
}

TEST_F(RuntimeRobotsTest, MissingFileIsEmptyWithoutProblems)
{
    const auto loaded = load_runtime_robots(path);
    EXPECT_TRUE(loaded.robots.empty());
    EXPECT_TRUE(loaded.problems.empty());
}

TEST_F(RuntimeRobotsTest, RoundTripKeepsEveryField)
{
    RobotSpec a = robot("tb3_3", "0003");
    a.responsive_wait = true;
    a.rotation = 0.5;
    a.scale = 2.0;
    a.tx = 1.5;
    a.ty = -3.0;
    std::string error;
    ASSERT_TRUE(save_runtime_robots(path, {a, robot("tb3_4", "0004")}, &error)) << error;

    const auto loaded = load_runtime_robots(path);
    EXPECT_TRUE(loaded.problems.empty());
    ASSERT_EQ(loaded.robots.size(), 2u);
    const auto &r = loaded.robots[0].name == "tb3_3" ? loaded.robots[0] : loaded.robots[1];
    EXPECT_EQ(r.manufacturer, "ROBOTIS");
    EXPECT_EQ(r.serial, "0003") << "the leading zeros must survive";
    EXPECT_EQ(r.charger, "charger_1");
    EXPECT_TRUE(r.responsive_wait);
    EXPECT_DOUBLE_EQ(r.rotation, 0.5);
    EXPECT_DOUBLE_EQ(r.scale, 2.0);
    EXPECT_DOUBLE_EQ(r.tx, 1.5);
    EXPECT_DOUBLE_EQ(r.ty, -3.0);
    EXPECT_FALSE(r.from_config);
}

TEST_F(RuntimeRobotsTest, SavingLeavesNoTemporaryFile)
{
    ASSERT_TRUE(save_runtime_robots(path, {robot("a", "1")}, nullptr));
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
    ASSERT_TRUE(save_runtime_robots(path, {}, nullptr));
    EXPECT_TRUE(load_runtime_robots(path).robots.empty());
}

TEST_F(RuntimeRobotsTest, UnwritableDirectoryFailsWithAnError)
{
    std::string error;
    EXPECT_FALSE(save_runtime_robots((dir / "missing" / "x.yaml").string(), {robot("a", "1")}, &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(RuntimeRobotsTest, CorruptFileIsReportedNotThrown)
{
    write("robots: [this is: not: valid");
    const auto loaded = load_runtime_robots(path);
    EXPECT_TRUE(loaded.robots.empty());
    ASSERT_EQ(loaded.problems.size(), 1u);
}

TEST_F(RuntimeRobotsTest, BadEntriesAreSkippedAndTheRestIsKept)
{
    write("robots:\n"
          "  good:\n    manufacturer: ROBOTIS\n    serial: \"0009\"\n    charger: charger_2\n"
          "  no_charger:\n    manufacturer: ROBOTIS\n    serial: \"0010\"\n"
          "  not_a_map: 5\n"
          "  bad_number:\n    manufacturer: ROBOTIS\n    serial: \"0011\"\n    charger: charger_1\n"
          "    transform: {scale: abc}\n");
    const auto loaded = load_runtime_robots(path);
    ASSERT_EQ(loaded.robots.size(), 1u);
    EXPECT_EQ(loaded.robots[0].name, "good");
    EXPECT_EQ(loaded.problems.size(), 3u);
}

TEST_F(RuntimeRobotsTest, WrongTopLevelShapeIsReported)
{
    write("robots: [a, b]\n");
    const auto loaded = load_runtime_robots(path);
    EXPECT_TRUE(loaded.robots.empty());
    EXPECT_EQ(loaded.problems.size(), 1u);
}

TEST_F(RuntimeRobotsTest, BackupKeepsTheCurrentFileAsBak)
{
    write("robots:\n  keep_me:\n    manufacturer: X\n");
    std::string error;
    ASSERT_TRUE(backup_runtime_robots(path, &error)) << error;
    ASSERT_TRUE(save_runtime_robots(path, {robot("tb3_2", "0002")}, &error)) << error;

    std::ifstream backup(path + ".bak");
    std::string text((std::istreambuf_iterator<char>(backup)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("keep_me"), std::string::npos) << "the backup has the old content";
    EXPECT_EQ(load_runtime_robots(path).robots.size(), 1u) << "the file itself has the new content";
}

TEST_F(RuntimeRobotsTest, BackupOfAMissingFileIsNotAnError)
{
    std::string error;
    EXPECT_TRUE(backup_runtime_robots(path, &error)) << error;
    EXPECT_FALSE(std::filesystem::exists(path + ".bak"));
}

TEST_F(RuntimeRobotsTest, ARewrittenFileKeepsItsMode)
{
    write("robots: {}\n");
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                           std::filesystem::perms::group_read);
    std::string error;
    ASSERT_TRUE(save_runtime_robots(path, {robot("tb3_2", "0002")}, &error)) << error;
    EXPECT_EQ(std::filesystem::status(path).permissions() & std::filesystem::perms::mask,
              std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read);

    ASSERT_TRUE(backup_runtime_robots(path, &error)) << error;
    EXPECT_EQ(std::filesystem::status(path + ".bak").permissions() & std::filesystem::perms::mask,
              std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read);
}
