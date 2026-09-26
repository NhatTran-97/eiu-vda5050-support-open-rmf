#ifndef LEVEL_MAPS_HPP
#define LEVEL_MAPS_HPP

#include <map>
#include <string>

#include "vda5050_fleet_adapter_full_control/rmf/transform.hpp"

namespace vda5050_fleet_adapter_full_control::rmf {

// Robot map of one RMF level.
struct LevelMap
{
    std::string map_id;
    Transform transform;
};

// RMF level <-> VDA5050 mapId and frame of one robot; unlisted levels keep their name and use the default frame.
class LevelMaps
{
public:
    explicit LevelMaps(const Transform &fallback = Transform()) : _fallback(fallback) {}

    // Add or replace the map of one level.
    void set(const std::string &level, const LevelMap &map) { _levels[level] = map; }

    // VDA5050 mapId of an RMF level.
    std::string map_id(const std::string &level) const
    {
        const auto it = _levels.find(level);
        return it == _levels.end() ? level : it->second.map_id;
    }

    // RMF level of a VDA5050 mapId.
    std::string level(const std::string &map_id) const
    {
        for (const auto &[name, map] : _levels)
        {
            if (map.map_id == map_id)
            {
                return name;
            }
        }
        return map_id;
    }

    // Frame of an RMF level.
    const Transform &transform(const std::string &level) const
    {
        const auto it = _levels.find(level);
        return it == _levels.end() ? _fallback : it->second.transform;
    }

private:
    Transform _fallback;
    std::map<std::string, LevelMap> _levels;
};

}  // namespace vda5050_fleet_adapter_full_control::rmf

#endif  // LEVEL_MAPS_HPP
