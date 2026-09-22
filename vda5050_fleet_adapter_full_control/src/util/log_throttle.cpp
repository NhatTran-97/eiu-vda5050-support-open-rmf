#include "vda5050_fleet_adapter_full_control/util/log_throttle.hpp"

#include <iterator>

namespace vda5050_fleet_adapter_full_control::util {

namespace {

// Key of the entry that keys beyond the limit share.
constexpr const char *kOverflowKey = "\x01overflow";

}  // namespace

LogThrottle::LogThrottle(std::chrono::seconds interval, std::size_t max_keys)
    : _interval(interval), _max_keys(max_keys > 0 ? max_keys : 1)
{
}

std::optional<std::size_t> LogThrottle::admit(const std::string &key, std::chrono::steady_clock::time_point now)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _entries.find(key);
    if (it == _entries.end())
    {
        if (_entries.size() >= _max_keys)
        {
            for (auto entry = _entries.begin(); entry != _entries.end();)
            {
                entry = now - entry->second.last >= _interval ? _entries.erase(entry) : std::next(entry);
            }
        }
        if (_entries.size() < _max_keys)
        {
            Entry fresh;
            fresh.last = now;
            _entries.emplace(key, fresh);
            return 0;
        }
        it = _entries.try_emplace(kOverflowKey).first;
    }

    Entry &entry = it->second;
    if (now - entry.last >= _interval)
    {
        const std::size_t held = entry.held;
        entry.last = now;
        entry.held = 0;
        return held;
    }
    ++entry.held;
    return std::nullopt;
}

}  // namespace vda5050_fleet_adapter_full_control::util
