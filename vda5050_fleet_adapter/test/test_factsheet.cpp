#include <gtest/gtest.h>

#include "vda5050_fleet_adapter/factsheet.hpp"

namespace proto = vda5050_fleet_adapter::protocol;

namespace {

proto::ParsedFactsheet sample()
{
  return proto::ParsedFactsheet(nlohmann::json::parse(R"({
    "typeSpecification": {"seriesName": "TurtleBot3 Burger"},
    "physicalParameters": {"speedMax": 0.22},
    "protocolFeatures": {"agvActions": [
      {"actionType": "pick", "blockingTypes": ["SOFT", "HARD"], "actionScopes": ["INSTANT"]},
      {"actionType": "cancelOrder", "blockingTypes": ["NONE"]}
    ]}
  })"));
}

}  // namespace

TEST(Factsheet, ParsesSeriesSpeedAndActions)
{
  const auto f = sample();
  EXPECT_EQ(f.series_name, "TurtleBot3 Burger");
  ASSERT_TRUE(f.speed_max.has_value());
  EXPECT_DOUBLE_EQ(*f.speed_max, 0.22);
  EXPECT_TRUE(f.supports_action("pick"));
  EXPECT_FALSE(f.supports_action("drop"));
  EXPECT_TRUE(f.has_content());
}

TEST(Factsheet, EmptyOrMalformedInputHasNoContent)
{
  EXPECT_FALSE(proto::ParsedFactsheet().has_content());
  EXPECT_FALSE(proto::ParsedFactsheet(nlohmann::json::parse("[]")).has_content());
}

TEST(Factsheet, BlockingTypePrefersRequestedThenFirstDeclared)
{
  const auto f = sample();
  EXPECT_EQ(f.blocking_type_for("pick", "HARD"), "HARD");
  EXPECT_EQ(f.blocking_type_for("pick", "NONE"), "SOFT");
  EXPECT_EQ(f.blocking_type_for("cancelOrder", "HARD"), "NONE");
  EXPECT_EQ(f.blocking_type_for("unknown", "HARD"), "HARD");
}

TEST(Factsheet, CustomActionMissingFromFactsheetIsRejected)
{
  const std::optional<proto::ParsedFactsheet> f = sample();
  EXPECT_EQ(proto::check_instant_action("pick", f).result, proto::ActionCheck::allowed);
  EXPECT_EQ(proto::check_instant_action("drop", f).result, proto::ActionCheck::reject);
}

TEST(Factsheet, CoreActionMissingFromFactsheetOnlyWarns)
{
  const std::optional<proto::ParsedFactsheet> f = sample();
  EXPECT_EQ(proto::check_instant_action("startPause", f).result, proto::ActionCheck::warn);
  EXPECT_EQ(proto::check_instant_action("initPosition", f).result, proto::ActionCheck::warn);
}

TEST(Factsheet, BlockingActionWhileDrivingConflicts)
{
  const std::optional<proto::ParsedFactsheet> f = sample();
  EXPECT_EQ(proto::action_conflicts("pick", "HARD", true, {}, f).size(), 1u);
  EXPECT_TRUE(proto::action_conflicts("pick", "NONE", true, {}, f).empty());
  EXPECT_TRUE(proto::action_conflicts("pick", "HARD", false, {}, f).empty());
}

TEST(Factsheet, RunningHardOnlyActionConflictsWithOthers)
{
  const std::optional<proto::ParsedFactsheet> f = proto::ParsedFactsheet(nlohmann::json::parse(R"({
    "protocolFeatures": {"agvActions": [
      {"actionType": "lift", "blockingTypes": ["HARD"]},
      {"actionType": "beep", "blockingTypes": ["NONE", "HARD"]}
    ]}
  })"));
  const auto running = [](const char* type, const char* status) {
    return std::vector<nlohmann::json>{
      {{"actionId", "a1"}, {"actionType", type}, {"actionStatus", status}}};
  };

  EXPECT_EQ(proto::action_conflicts("pick", "NONE", false, running("lift", "RUNNING"), f).size(), 1u);
  EXPECT_TRUE(proto::action_conflicts("lift", "NONE", false, running("lift", "RUNNING"), f).empty());
  EXPECT_TRUE(proto::action_conflicts("pick", "NONE", false, running("lift", "FINISHED"), f).empty());
  EXPECT_TRUE(proto::action_conflicts("pick", "NONE", false, running("beep", "RUNNING"), f).empty());
  EXPECT_TRUE(proto::action_conflicts("pick", "NONE", false, running("lift", "RUNNING"), std::nullopt).empty());
}

TEST(Factsheet, NoFactsheetAllowsEverything)
{
  EXPECT_EQ(proto::check_instant_action("drop", std::nullopt).result, proto::ActionCheck::allowed);
}
