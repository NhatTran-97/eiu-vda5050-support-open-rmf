#ifndef LOG_THROTTLE_HPP
#define LOG_THROTTLE_HPP

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace vda5050_fleet_adapter_full_control::util {

// Lets one message per key through in each interval and counts the ones it held back.
class LogThrottle
{
public:
    // Keys beyond `max_keys` share a single entry.
    explicit LogThrottle(std::chrono::seconds interval, std::size_t max_keys = 256);

    // Whether a message for `key` may be logged at `now`, and how many were held back since the last one logged.
    std::optional<std::size_t> admit(const std::string &key, std::chrono::steady_clock::time_point now);

private:
    struct Entry
    {
        std::chrono::steady_clock::time_point last{};
        std::size_t held = 0;
    };

    std::chrono::seconds _interval;
    std::size_t _max_keys;
    std::mutex _mutex;
    std::unordered_map<std::string, Entry> _entries;
};

}  // namespace vda5050_fleet_adapter_full_control::util

#endif  // LOG_THROTTLE_HPP
