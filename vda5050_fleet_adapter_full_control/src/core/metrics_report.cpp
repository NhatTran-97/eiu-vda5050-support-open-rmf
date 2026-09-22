#include "vda5050_fleet_adapter_full_control/core/metrics_report.hpp"

#include <cstdint>
#include <cstdio>

namespace vda5050_fleet_adapter_full_control::core {

namespace {

using json = nlohmann::json;

// The whole number at JSON pointer `path` of `report`, or zero when it is missing.
std::uint64_t count_at(const json &report, const char *path)
{
    const json::json_pointer pointer(path);
    if (!report.contains(pointer) || !report.at(pointer).is_number())
    {
        return 0;
    }
    return report.at(pointer).get<std::uint64_t>();
}

// The real number at JSON pointer `path` of `report`, or zero when it is missing.
double number_at(const json &report, const char *path)
{
    const json::json_pointer pointer(path);
    if (!report.contains(pointer) || !report.at(pointer).is_number())
    {
        return 0.0;
    }
    return report.at(pointer).get<double>();
}

// How much the count at `path` grew from `before` to `now`.
std::uint64_t grew(const json &now, const json &before, const char *path)
{
    const std::uint64_t current = count_at(now, path);
    const std::uint64_t earlier = count_at(before, path);
    return current >= earlier ? current - earlier : current;
}

// A duration given in microseconds as text in the unit that reads best.
std::string duration_text(double microseconds)
{
    char text[32];
    if (microseconds >= 1e6)
    {
        std::snprintf(text, sizeof(text), "%.2f s", microseconds / 1e6);
    }
    else if (microseconds >= 1e3)
    {
        std::snprintf(text, sizeof(text), "%.1f ms", microseconds / 1e3);
    }
    else
    {
        std::snprintf(text, sizeof(text), "%.0f us", microseconds);
    }
    return text;
}

// "<label> n=<count> p50 <..> p99 <..> max <..>" for the distribution at `path`, or "<label> idle" when it recorded nothing.
std::string distribution_text(const json &report, const std::string &label, const std::string &path)
{
    const auto count = count_at(report, (path + "/count").c_str());
    if (count == 0)
    {
        return label + " idle";
    }
    return label + " n=" + std::to_string(count) + " p50 " + duration_text(number_at(report, (path + "/p50").c_str())) + " p99 " +
           duration_text(number_at(report, (path + "/p99").c_str())) + " max " + duration_text(number_at(report, (path + "/max").c_str()));
}

}  // namespace

std::string metrics_summary(const json &now, const json &before)
{
    char head[160];
    std::snprintf(head, sizeof(head), "[metrics] %.0f s | robots %llu/%llu online, state age max %.2f s", number_at(now, "/interval_s"),
                  static_cast<unsigned long long>(count_at(now, "/robots/online")),
                  static_cast<unsigned long long>(count_at(now, "/robots/registered")), number_at(now, "/robots/state_age_max_s"));
    std::string line = head;
    const json::json_pointer oldest_pointer("/robots/oldest_state_robot");
    const std::string oldest = now.contains(oldest_pointer) && now.at(oldest_pointer).is_string() ? now.at(oldest_pointer).get<std::string>() : std::string{};
    if (!oldest.empty())
    {
        line += " (" + oldest + ")";
    }

    line += " | rx state +" + std::to_string(grew(now, before, "/rx/state")) + " viz +" + std::to_string(grew(now, before, "/rx/visualization")) +
            " conn +" + std::to_string(grew(now, before, "/rx/connection")) + " fact +" + std::to_string(grew(now, before, "/rx/factsheet")) +
            ", other robots +" + std::to_string(grew(now, before, "/rx/unregistered"));

    std::uint64_t dropped = 0;
    for (const char *kind : {"bad_payload", "bad_topic", "invalid_state", "stale_state", "oversize"})
    {
        dropped += grew(now, before, (std::string("/dropped/") + kind).c_str());
    }
    line += " | dropped +" + std::to_string(dropped) + " (stale +" + std::to_string(grew(now, before, "/dropped/stale_state")) + ", oversize +" +
            std::to_string(grew(now, before, "/dropped/oversize")) + ", log lines held back +" + std::to_string(grew(now, before, "/log_suppressed")) + ")";

    line += " | " + distribution_text(now, "handle state", "/latency_us/handle_state");
    line += " | " + distribution_text(now, "mutex wait", "/latency_us/mutex_wait");
    line += " | " + distribution_text(now, "loop pass", "/update_loop/pass_us") + " overruns +" + std::to_string(grew(now, before, "/update_loop/overruns"));
    line += " | published +" + std::to_string(grew(now, before, "/published/ok")) + " failed +" + std::to_string(grew(now, before, "/published/failed")) +
            ", mqtt lost +" + std::to_string(grew(now, before, "/mqtt/connections_lost"));
    return line;
}

MetricsReporter::MetricsReporter(rclcpp::Node &node, std::chrono::duration<double> period, Collect collect)
    : _node(node), _collect(std::move(collect)), _started(std::chrono::steady_clock::now()), _last(_started)
{
    if (!(period.count() > 0.0))
    {
        return;
    }
    _publisher = _node.create_publisher<std_msgs::msg::String>("~/metrics", rclcpp::QoS(10));
    _timer = _node.create_wall_timer(std::chrono::duration_cast<std::chrono::nanoseconds>(period), [this] { report(); });
}

void MetricsReporter::report()
{
    json report = _collect();
    const auto now = std::chrono::steady_clock::now();
    report["uptime_s"] = std::chrono::duration<double>(now - _started).count();
    report["interval_s"] = std::chrono::duration<double>(now - _last).count();
    _last = now;

    std_msgs::msg::String message;
    message.data = report.dump();
    _publisher->publish(message);
    RCLCPP_INFO(_node.get_logger(), "%s", metrics_summary(report, _previous).c_str());
    _previous = std::move(report);
}

}  // namespace vda5050_fleet_adapter_full_control::core
