#ifndef METRICS_REPORT_HPP
#define METRICS_REPORT_HPP

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>

namespace vda5050_fleet_adapter_full_control::core {

// Summary log line of a report: robots online, messages and drops since `before` (empty for the first report), interval latency.
std::string metrics_summary(const nlohmann::json &now, const nlohmann::json &before);

// Every `period` publishes the adapter's metrics as JSON on "~/metrics" and logs their summary line.
class MetricsReporter
{
public:
    using Collect = std::function<nlohmann::json()>;

    // The reporter adds "uptime_s" and "interval_s" to what `collect` builds; a period <= 0 reports nothing.
    MetricsReporter(rclcpp::Node &node, std::chrono::duration<double> period, Collect collect);

private:
    void report();

    rclcpp::Node &_node;
    Collect _collect;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr _publisher;
    rclcpp::TimerBase::SharedPtr _timer;
    std::chrono::steady_clock::time_point _started;
    std::chrono::steady_clock::time_point _last;
    nlohmann::json _previous = nlohmann::json::object();
};

}  // namespace vda5050_fleet_adapter_full_control::core

#endif  // METRICS_REPORT_HPP
