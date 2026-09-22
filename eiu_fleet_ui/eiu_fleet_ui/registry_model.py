"""Model of robot registration: what each fleet has and which robots wait to be registered.

Pure Python. The fleet adapters publish the facts and decide who may join; this module only merges them.
"""

import re
from dataclasses import dataclass, field

from .config import RobotIdentity

# A robot name the adapter accepts: 1-64 letters, digits, '_' or '-', starting with a letter or digit.
_NAME_UNSAFE = re.compile(r"[^A-Za-z0-9_-]")
_NAME_MAX_LENGTH = 64
_TRAILING_NUMBER = re.compile(r"^(.*?)(\d+)$")


def robot_key(manufacturer: str, serial: str) -> str:
    """Broker identity of a robot, the way the adapters key it."""
    return f"{manufacturer}/{serial}"


@dataclass
class Change:
    """Robots that started or stopped being followed after a registry message."""

    added: list = field(default_factory=list)      # RobotIdentity
    removed: list = field(default_factory=list)    # robot names


class RegistryModel:
    """Latest registry of every fleet plus the robots each fleet sees but has not registered."""

    def __init__(self, configured=()):
        self._configured = {r.name: r for r in configured}
        self._followed = dict(self._configured)
        # Names followed because a registry listed them, not because the config did.
        self._from_registry: set = set()
        self._fleets: dict = {}
        self._snapshots: dict = {}
        self._dismissed: set = set()
        self._announced: set = set()
        # (robot name, fleet) -> fleet whose robot of that name is followed instead.
        self._conflicts: dict = {}

    # Messages from the adapters.

    def apply_registry(self, registry) -> Change:
        """Take a fleet's registry and follow its robots; returns what changed."""
        if (not isinstance(registry, dict) or not isinstance(registry.get("fleet"), str)
                or not isinstance(registry.get("robots"), list)):
            return Change()
        fleet = registry["fleet"]
        self._fleets[fleet] = registry
        self._conflicts = {key: kept for key, kept in self._conflicts.items() if key[1] != fleet}

        change = Change()
        listed = set()
        for robot in registry["robots"]:
            if not isinstance(robot, dict) or not isinstance(robot.get("name"), str):
                continue
            name = robot["name"]
            if robot.get("retired"):
                continue
            listed.add(name)
            if name in self._followed:
                if self._followed[name].fleet_name != fleet:
                    self._conflicts[(name, fleet)] = self._followed[name].fleet_name
                continue
            identity = RobotIdentity(
                name=name,
                manufacturer=str(robot.get("manufacturer", "")),
                serial=str(robot.get("serial", "")),
                interface_name=str(registry.get("interface", "")),
                adapter_node=str(registry.get("adapter_node") or ""),
                fleet_name=fleet)
            self._followed[name] = identity
            self._from_registry.add(name)
            change.added.append(identity)

        # A robot the registry followed earlier and this fleet no longer lists is gone.
        for name in sorted(self._from_registry):
            identity = self._followed.get(name)
            if identity is not None and identity.fleet_name == fleet and name not in listed:
                del self._followed[name]
                self._from_registry.discard(name)
                change.removed.append(name)
        return change

    def apply_discovery(self, snapshot) -> None:
        """Take a fleet adapter's list of robots that are online but registered nowhere."""
        if (not isinstance(snapshot, dict) or not isinstance(snapshot.get("reporter"), str)
                or not isinstance(snapshot.get("robots"), list)):
            return
        self._snapshots[snapshot["reporter"]] = [
            r for r in snapshot["robots"]
            if isinstance(r, dict) and isinstance(r.get("manufacturer"), str) and isinstance(r.get("serial"), str)]

    def dismiss(self, key: str) -> None:
        """Stop offering a robot for registration until the dashboard restarts."""
        self._dismissed.add(key)

    # What the dashboard follows.

    def followed(self) -> list:
        """Configured robots first, then the ones registries added, each as a RobotIdentity."""
        configured = [r for r in self._followed.values() if r.name in self._configured]
        added = [r for r in self._followed.values() if r.name not in self._configured]
        return configured + added

    def source_of(self, name: str) -> str:
        """'config' or 'runtime' for a robot a registry lists; '' if none does."""
        for registry in self._fleets.values():
            for robot in registry["robots"]:
                if isinstance(robot, dict) and robot.get("name") == name:
                    return str(robot.get("source", ""))
        return ""

    def conflicts(self) -> list:
        """Robot names that two fleets both use; only one of them can be followed."""
        return [{"name": name, "fleet": fleet, "followed_fleet": kept}
                for (name, fleet), kept in sorted(self._conflicts.items())]

    def fleets(self) -> list:
        """Every fleet's latest registry, by fleet name."""
        return [self._fleets[name] for name in sorted(self._fleets)]

    # Robots waiting to be registered.

    def _known_keys(self) -> set:
        keys = {robot_key(r.manufacturer, r.serial) for r in self._followed.values()}
        for registry in self._fleets.values():
            for robot in registry["robots"]:
                # A removed robot is registered nowhere, so it may be offered again.
                if isinstance(robot, dict) and not robot.get("retired"):
                    keys.add(robot_key(str(robot.get("manufacturer", "")), str(robot.get("serial", ""))))
        return keys

    def pending(self) -> list:
        """Robots online on the broker that no fleet has registered, one entry each."""
        known = self._known_keys()
        merged: dict = {}
        for reporter, robots in self._snapshots.items():
            for robot in robots:
                key = robot_key(robot["manufacturer"], robot["serial"])
                if key in known or key in self._dismissed:
                    continue
                entry = merged.setdefault(key, {
                    "key": key,
                    "manufacturer": robot["manufacturer"],
                    "serial": robot["serial"],
                    "series": robot.get("series") or "",
                    "kinematic": robot.get("kinematic") or "",
                    "speed_max": robot.get("speed_max"),
                    "pose": robot.get("pose"),
                    "removed_as": None,
                    "reporters": [],
                })
                entry["reporters"].append(reporter)
                if entry["removed_as"] is None:
                    entry["removed_as"] = self._removed_as_of(robot)
        result = [merged[key] for key in sorted(merged)]
        for entry in result:
            removed = entry["removed_as"]
            if removed and not entry["series"]:
                entry["series"] = str(self._fleets.get(removed["fleet"], {}).get("series") or "")
            entry["suggested_fleet"] = removed["fleet"] if removed else self.suggest_fleet(entry)
            # Fleets that run robots of this type, whether or not they see this robot.
            matching = {name for name, registry in self._fleets.items()
                        if entry["series"] and registry.get("series") == entry["series"]}
            if removed:
                matching.add(removed["fleet"])
            entry["matching_fleets"] = sorted(matching)
        return result

    @staticmethod
    def _removed_as_of(robot):
        """Fleet, name and charger a robot had before it was removed, when the adapter says so."""
        removed = robot.get("removed_as")
        keys = ("fleet", "name", "charger")
        if isinstance(removed, dict) and all(isinstance(removed.get(k), str) for k in keys):
            return {k: removed[k] for k in keys}
        return None

    def _removed_robot(self, fleet: str, manufacturer: str, serial: str):
        """What a removed robot of this identity was called and where it charged in `fleet`, or None."""
        for robots in self._snapshots.values():
            for robot in robots:
                removed = self._removed_as_of(robot)
                if (removed and removed["fleet"] == fleet and robot.get("manufacturer") == manufacturer
                        and robot.get("serial") == serial):
                    return removed
        return None

    def new_pending_keys(self) -> list:
        """Pending robots not announced before; a robot that left the list is announced again on return."""
        current = {entry["key"] for entry in self.pending()}
        fresh = sorted(current - self._announced)
        self._announced = current
        return fresh

    # Suggestions; they only prefill the form, the adapter still checks everything.

    def suggest_fleet(self, pending_entry) -> str:
        """The one fleet that saw the robot and has robots of its type, or '' when it is not clear."""
        series = pending_entry.get("series") or ""
        if not series:
            return ""
        matches = [name for name in pending_entry["reporters"]
                   if name in self._fleets and self._fleets[name].get("series") == series]
        return matches[0] if len(matches) == 1 else ""

    def suggest_name(self, fleet: str, manufacturer: str, serial: str) -> str:
        """The name a removed robot had, else a free one that continues the fleet's naming or is built from the identity."""
        removed = self._removed_robot(fleet, manufacturer, serial)
        if removed:
            return removed["name"]
        taken = set(self._followed)
        for registry in self._fleets.values():
            for robot in registry["robots"]:
                if isinstance(robot, dict) and isinstance(robot.get("name"), str):
                    taken.add(robot["name"])

        # Robots of this fleet whose names end in a number, grouped by what precedes it.
        prefixes: dict = {}
        registry = self._fleets.get(fleet)
        for robot in (registry["robots"] if registry is not None else []):
            match = _TRAILING_NUMBER.match(robot["name"]) if isinstance(robot, dict) and isinstance(robot.get("name"), str) else None
            if match:
                prefixes.setdefault(match.group(1), []).append(match.group(2))

        if prefixes:
            prefix = max(prefixes, key=lambda p: len(prefixes[p]))
            width = max(len(digits) for digits in prefixes[prefix])
            used = [int(m.group(2)) for name in taken for m in [_TRAILING_NUMBER.match(name)]
                    if m and m.group(1) == prefix]
            number = max(used) + 1
            while f"{prefix}{number:0{width}d}" in taken:
                number += 1
            return f"{prefix}{number:0{width}d}"

        base = _NAME_UNSAFE.sub("_", f"{manufacturer}_{serial}").lstrip("_-")[:_NAME_MAX_LENGTH] or "robot"
        name, n = base, 2
        while name in taken:
            suffix = f"_{n}"
            name = base[:_NAME_MAX_LENGTH - len(suffix)] + suffix
            n += 1
        return name

    def suggest_charger(self, fleet: str, manufacturer: str = "", serial: str = "") -> str:
        """The charger a removed robot of this identity had, else the first one of the fleet that no robot uses, or ''."""
        removed = self._removed_robot(fleet, manufacturer, serial)
        if removed:
            return removed["charger"]
        registry = self._fleets.get(fleet)
        if registry is None:
            return ""
        for charger in registry.get("chargers", []):
            if isinstance(charger, dict) and not charger.get("used_by") and isinstance(charger.get("name"), str):
                return charger["name"]
        return ""
