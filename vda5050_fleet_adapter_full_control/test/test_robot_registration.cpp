#include <gtest/gtest.h>

#include <cmath>

#include "vda5050_fleet_adapter_full_control/core/robot_registration.hpp"

using namespace vda5050_fleet_adapter_full_control;
using namespace vda5050_fleet_adapter_full_control::core;

namespace {

vda5050::ParsedFactsheet factsheet(const std::string &series, double speed, const std::string &kinematic = "DIFF",
                                   const std::string &agv_class = "CARRIER")
{
    return vda5050::ParsedFactsheet(nlohmann::json{
        {"typeSpecification", {{"seriesName", series}, {"agvKinematic", kinematic}, {"agvClass", agv_class}}},
        {"physicalParameters", {{"speedMax", speed}, {"length", 0.2}, {"width", 0.2}}},
    });
}

class RegistrationTest : public ::testing::Test
{
protected:
    RegistrationTest()
    {
        fleet.limits = {"tb3_fleet", 0.15, 0.22, 1.0, 0.05};
        fleet.robots = {{"tb3_fleet", "tb3_1", "ROBOTIS", "0001", "charger_2", false},
                        {"amr_fleet", "amr_1", "EIU-FABLAB", "0001", "charger_1", false}};
        fleet.reference_factsheets = {factsheet("TurtleBot3 Burger", 0.22)};

        graph.is_charger = [](const std::string &w) { return w == "charger_1" || w == "charger_2"; };
        graph.has_map = [](const std::string &m) { return m == "tb3_world"; };
        graph.on_graph = [](const std::string &, double x, double y, double) { return x > 0 && x < 30 && y < 0 && y > -12; };

        spec = {"tb3_3", "ROBOTIS", "0003", "charger_1", false, 0.0, 1.0, 0.0, 0.0, false};

        candidate.seen = true;
        candidate.online = true;
        candidate.factsheet = factsheet("TurtleBot3 Burger", 0.22);
        candidate.has_state = true;
        candidate.pose_initialized = true;
        candidate.x = 10.0;
        candidate.y = -6.0;
        candidate.map_id = "tb3_world";
    }

    Verdict check(bool confirm = false) { return validate_new_robot(spec, fleet, candidate, graph, confirm); }

    FleetView fleet;
    GraphFacts graph;
    RobotSpec spec;
    CandidateFacts candidate;
};

}  // namespace

TEST_F(RegistrationTest, MatchingRobotIsAcceptedWithASharedChargerWarning)
{
    const auto v = check();
    EXPECT_TRUE(v.ok());
    ASSERT_EQ(v.warnings.size(), 1u);
    EXPECT_EQ(v.warnings[0].code, "charger_shared");
}

TEST_F(RegistrationTest, NameMustBeSimpleAndUnused)
{
    for (const std::string &bad : std::vector<std::string>{"", "a b", "../x", "_x", "x/y", "x+", std::string(65, 'a')})
    {
        spec.name = bad;
        EXPECT_TRUE(check().has_error("name_invalid")) << "'" << bad << "'";
    }
    spec.name = "tb3_1";
    EXPECT_TRUE(check().has_error("name_taken"));
    spec.name = "amr_1";
    EXPECT_TRUE(check().has_error("name_taken")) << "names are unique across fleets";
}

TEST_F(RegistrationTest, IdentityMustBeValidAndUnused)
{
    spec.manufacturer = "ROBOTIS";
    spec.serial = "0001";
    EXPECT_TRUE(check().has_error("identity_taken"));

    fleet.robots[0].retired = true;
    const auto retired = check();
    ASSERT_TRUE(retired.has_error("identity_taken"));
    EXPECT_NE(retired.errors[0].message.find("restart"), std::string::npos);
    EXPECT_NE(retired.errors[0].message.find("register it again"), std::string::npos);

    for (const std::string &bad : std::vector<std::string>{"", "a/b", "a+b", "a#", "a b", std::string(65, '1')})
    {
        spec.serial = bad;
        EXPECT_TRUE(check().has_error("identity_invalid")) << "'" << bad << "'";
    }
    spec.serial = "0001";
    spec.manufacturer = "EIU-FABLAB";
    EXPECT_TRUE(check().has_error("identity_taken")) << "EIU-FABLAB/0001 belongs to amr_1 in another fleet";
}

TEST_F(RegistrationTest, TakenOrMalformedIdentityIsNotCheckedAgainstTheBroker)
{
    candidate = {};
    spec.serial = "0001";
    spec.manufacturer = "ROBOTIS";
    auto taken = check();
    EXPECT_TRUE(taken.has_error("identity_taken"));
    EXPECT_FALSE(taken.has_error("unverified")) << "a registered robot has no pending facts";

    spec.serial = "a/b";
    auto malformed = check();
    EXPECT_TRUE(malformed.has_error("identity_invalid"));
    EXPECT_FALSE(malformed.has_error("unverified"));
}

TEST_F(RegistrationTest, InvalidTransformDoesNotAlsoReportOffGraph)
{
    spec.scale = 0.0;
    const auto v = check();
    EXPECT_TRUE(v.has_error("transform_invalid"));
    EXPECT_FALSE(v.has_error("off_graph")) << "the pose cannot be converted with a broken transform";
}

TEST_F(RegistrationTest, SameSerialUnderAnotherManufacturerIsADifferentRobot)
{
    spec.manufacturer = "ACME";
    spec.serial = "0001";
    EXPECT_FALSE(check().has_error("identity_taken"));
}

TEST_F(RegistrationTest, TransformMustBeFiniteAndNonZeroScale)
{
    spec.scale = 0.0;
    EXPECT_TRUE(check().has_error("transform_invalid"));
    spec.scale = 1.0;
    spec.rotation = std::nan("");
    EXPECT_TRUE(check().has_error("transform_invalid"));
    spec.rotation = 0.0;
    spec.tx = INFINITY;
    EXPECT_TRUE(check().has_error("transform_invalid"));
}

TEST_F(RegistrationTest, ChargerMustExistAndBeUniqueInTheFleet)
{
    spec.charger = "";
    EXPECT_TRUE(check().has_error("charger_missing"));
    spec.charger = "Patrol_A1";
    EXPECT_TRUE(check().has_error("charger_unknown"));
    spec.charger = "charger_2";
    EXPECT_TRUE(check().has_error("charger_taken"));
    spec.charger = "charger_1";
    EXPECT_FALSE(check().has_error("charger_taken"));
}

TEST_F(RegistrationTest, RetiredRobotsKeepTheirChargerUntilRestart)
{
    fleet.robots[0].retired = true;
    spec.charger = "charger_2";
    const auto v = check();
    ASSERT_TRUE(v.has_error("charger_taken")) << "RMF still holds the removed robot's last position";
    EXPECT_NE(v.errors[0].message.find("restart"), std::string::npos);
}

TEST_F(RegistrationTest, UnseenRobotNeedsConfirmation)
{
    candidate = CandidateFacts{};
    const auto v = check();
    EXPECT_FALSE(v.ok());
    EXPECT_TRUE(v.needs_confirmation);
    ASSERT_EQ(v.errors.size(), 1u);
    EXPECT_EQ(v.errors[0].code, "unverified");

    const auto confirmed = check(true);
    EXPECT_TRUE(confirmed.ok());
    EXPECT_FALSE(confirmed.needs_confirmation);
    EXPECT_GE(confirmed.warnings.size(), 2u);
}

TEST_F(RegistrationTest, OfflineRobotIsUnverifiedNotRejected)
{
    candidate.online = false;
    const auto v = check();
    EXPECT_TRUE(v.needs_confirmation);
    EXPECT_TRUE(check(true).ok());
}

TEST_F(RegistrationTest, ConfirmationDoesNotOverrideRealErrors)
{
    candidate = CandidateFacts{};
    spec.charger = "nowhere";
    const auto v = check(true);
    EXPECT_FALSE(v.ok());
    EXPECT_TRUE(v.has_error("charger_unknown"));
    const auto unconfirmed = check(false);
    EXPECT_FALSE(unconfirmed.needs_confirmation) << "another error is present, so confirming would not help";
}

TEST_F(RegistrationTest, ARobotThatIsNotLocalizedCanBeAddedOnceConfirmed)
{
    candidate.pose_initialized = false;
    const auto v = check();
    EXPECT_FALSE(v.ok());
    EXPECT_TRUE(v.needs_confirmation) << "the operator can localize it from the dashboard after adding it";
    ASSERT_EQ(v.errors.size(), 1u);
    EXPECT_EQ(v.errors[0].code, "unverified");
    EXPECT_NE(v.errors[0].message.find("not localized"), std::string::npos);

    const auto confirmed = check(true);
    EXPECT_TRUE(confirmed.ok());
    bool noted = false;
    for (const auto &w : confirmed.warnings)
    {
        noted = noted || (w.code == "unverified" && w.message.find("not localized") != std::string::npos);
    }
    EXPECT_TRUE(noted);
}

TEST_F(RegistrationTest, PoseMustBeOnAKnownMapAndOnTheGraph)
{

    candidate.map_id = "other_floor";
    EXPECT_TRUE(check().has_error("map_unknown"));
    candidate.map_id = "tb3_world";

    candidate.x = 500.0;
    EXPECT_TRUE(check().has_error("off_graph"));
}

TEST_F(RegistrationTest, GraphCheckUsesThePoseInTheRmfFrame)
{
    // Robot frame = RMF frame + (100, 0): the raw pose is far off the graph, the RMF pose is on it.
    spec.tx = 100.0;
    candidate.x = 110.0;
    EXPECT_FALSE(check().has_error("off_graph"));
    spec.tx = 0.0;
    EXPECT_TRUE(check().has_error("off_graph"));
}

TEST_F(RegistrationTest, RobotSlowerThanTheFleetPlansForIsRejected)
{
    candidate.factsheet = factsheet("TurtleBot3 Burger", 0.10);
    EXPECT_TRUE(check().has_error("speed_too_low"));
    candidate.factsheet = factsheet("TurtleBot3 Burger", 0.21);
    EXPECT_FALSE(check().has_error("speed_too_low")) << "within tolerance";
}

TEST_F(RegistrationTest, ToleranceComesFromTheFleetLimits)
{
    candidate.factsheet = factsheet("TurtleBot3 Burger", 0.21);
    fleet.limits.tolerance = 0.0;
    EXPECT_TRUE(check().has_error("speed_too_low"));
    fleet.limits.tolerance = 0.10;
    EXPECT_FALSE(check().has_error("speed_too_low"));

    candidate.factsheet = factsheet("TurtleBot3 Burger", 0.25);
    const auto has_warning = [&](const std::string &code)
    {
        for (const auto &w : check().warnings)
        {
            if (w.code == code)
            {
                return true;
            }
        }
        return false;
    };
    fleet.limits.tolerance = 0.05;
    EXPECT_TRUE(has_warning("speed_higher"));
    fleet.limits.tolerance = 0.20;
    EXPECT_FALSE(has_warning("speed_higher"));
}

TEST_F(RegistrationTest, RobotFasterThanTheFleetPlansForOnlyWarns)
{
    candidate.factsheet = factsheet("TurtleBot3 Burger", 0.50);
    const auto v = check();
    EXPECT_TRUE(v.ok());
    bool warned = false;
    for (const auto &w : v.warnings)
    {
        warned = warned || w.code == "speed_higher";
    }
    EXPECT_TRUE(warned);
}

TEST_F(RegistrationTest, RobotLargerThanTheFootprintIsRejected)
{
    candidate.factsheet = vda5050::ParsedFactsheet(nlohmann::json{
        {"typeSpecification", {{"seriesName", "TurtleBot3 Burger"}, {"agvKinematic", "DIFF"}, {"agvClass", "CARRIER"}}},
        {"physicalParameters", {{"speedMax", 0.22}, {"length", 0.8}, {"width", 0.5}}},
    });
    EXPECT_TRUE(check().has_error("too_large"));
}

TEST_F(RegistrationTest, DifferentTypeIsRejected)
{
    candidate.factsheet = factsheet("AGV", 0.22);
    EXPECT_TRUE(check().has_error("type_mismatch"));
    candidate.factsheet = factsheet("TurtleBot3 Burger", 0.22, "OMNI");
    EXPECT_TRUE(check().has_error("type_mismatch")) << "kinematic differs";
    candidate.factsheet = factsheet("TurtleBot3 Burger", 0.22, "DIFF", "FORKLIFT");
    EXPECT_TRUE(check().has_error("type_mismatch")) << "class differs";
}

TEST_F(RegistrationTest, MissingFactsheetDetailsAreUnverified)
{
    candidate.factsheet = vda5050::ParsedFactsheet(nlohmann::json{
        {"typeSpecification", {{"seriesName", "TurtleBot3 Burger"}}},
    });
    const auto v = check();
    EXPECT_TRUE(v.needs_confirmation);
    EXPECT_TRUE(check(true).ok());

    candidate.factsheet.reset();
    EXPECT_TRUE(check().needs_confirmation);
}

TEST_F(RegistrationTest, NoReferenceRobotMeansTheTypeIsUnverified)
{
    fleet.reference_factsheets.clear();
    const auto v = check();
    EXPECT_TRUE(v.needs_confirmation);
    ASSERT_EQ(v.errors.size(), 1u);
    EXPECT_NE(v.errors[0].message.find("compare its type"), std::string::npos);
}

TEST_F(RegistrationTest, SpecChecksAloneNeedNoBrokerData)
{
    EXPECT_TRUE(validate_spec(spec, fleet, graph).ok());
    spec.charger = "x";
    EXPECT_TRUE(validate_spec(spec, fleet, graph).has_error("charger_unknown"));
}

TEST_F(RegistrationTest, ARemovedRobotCanBeRestoredOnlyAsTheSameRobot)
{
    // What the manager holds for a robot removed earlier in this session.
    const RobotSpec removed = {"tb3_2", "ROBOTIS", "0002", "charger_1", true, 0.5, 1.2, 1.0, -2.0, false};
    EXPECT_TRUE(same_robot(removed, removed));

    RobotSpec other = removed;
    other.name = "tb3_9";
    EXPECT_FALSE(same_robot(removed, other)) << "another name is another robot";
    other = removed;
    other.serial = "0009";
    EXPECT_FALSE(same_robot(removed, other));
    other = removed;
    other.charger = "charger_2";
    EXPECT_FALSE(same_robot(removed, other)) << "RMF holds its last position at the old charger";
    other = removed;
    other.responsive_wait = false;
    EXPECT_FALSE(same_robot(removed, other));
    other = removed;
    other.rotation += 0.01;
    EXPECT_FALSE(same_robot(removed, other));
    other = removed;
    other.tx += 1e-12;
    EXPECT_TRUE(same_robot(removed, other)) << "a value that went through a file may differ in the last digits";
}

TEST_F(RegistrationTest, RefusalOfARemovedRobotSaysHowToGetItBack)
{
    fleet.robots[0].retired = true;
    spec = {"tb3_1", "ROBOTIS", "0002", "charger_2", false, 0.0, 1.0, 0.0, 0.0, false};
    const auto v = validate_spec(spec, fleet, graph);
    ASSERT_TRUE(v.has_error("name_taken"));
    EXPECT_NE(v.errors[0].message.find("register it again with the same settings"), std::string::npos);
}

