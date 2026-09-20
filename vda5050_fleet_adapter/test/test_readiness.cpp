#include <gtest/gtest.h>

#include "vda5050_fleet_adapter/readiness.hpp"

using vda5050_fleet_adapter::evaluate_readiness;
namespace proto = vda5050_fleet_adapter::protocol;

namespace {

proto::ParsedState state_with(const std::string& extra = "")
{
  const std::string base = R"("agvPosition": {"x": 1.0, "y": 2.0, "theta": 0.0,
                              "mapId": "m", "positionInitialized": true})";
  return proto::ParsedState(nlohmann::json::parse("{" + base + (extra.empty() ? "" : "," + extra) + "}"));
}

}  // namespace

TEST(Readiness, HealthyRobotIsReady)
{
  EXPECT_TRUE(evaluate_readiness(true, state_with()).ready);
}

TEST(Readiness, OfflineOrWithoutStateIsNotReady)
{
  EXPECT_EQ(evaluate_readiness(false, state_with()).reason, "offline");
  EXPECT_EQ(evaluate_readiness(true, std::nullopt).reason, "no state received");
}

TEST(Readiness, UninitializedPoseIsNotReady)
{
  const proto::ParsedState s(nlohmann::json::parse(
    R"({"agvPosition": {"x": 1, "y": 2, "theta": 0, "positionInitialized": false}})"));
  EXPECT_EQ(evaluate_readiness(true, s).reason, "no valid pose");
}

TEST(Readiness, ManualModeEStopAndFatalErrorAreNotReady)
{
  EXPECT_FALSE(evaluate_readiness(true, state_with(R"("operatingMode": "MANUAL")")).ready);
  EXPECT_FALSE(evaluate_readiness(
    true, state_with(R"("safetyState": {"eStop": "AUTOACK", "fieldViolation": false})")).ready);
  const auto fatal = evaluate_readiness(
    true, state_with(R"("errors": [{"errorType": "motorFault", "errorLevel": "FATAL"}])"));
  EXPECT_FALSE(fatal.ready);
  EXPECT_NE(fatal.reason.find("motorFault"), std::string::npos);
}

TEST(Readiness, WarningErrorsAndSemiAutomaticModeAreStillReady)
{
  EXPECT_TRUE(evaluate_readiness(
    true, state_with(R"("errors": [{"errorType": "lowBattery", "errorLevel": "WARNING"}])")).ready);
  EXPECT_TRUE(evaluate_readiness(true, state_with(R"("operatingMode": "SEMIAUTOMATIC")")).ready);
}

TEST(Readiness, PauseBlocksOnlyWhenNotTolerated)
{
  const auto s = state_with(R"("paused": true)");
  EXPECT_EQ(evaluate_readiness(true, s).reason, "AGV reports paused");
  EXPECT_TRUE(evaluate_readiness(true, s, true).ready);
}
