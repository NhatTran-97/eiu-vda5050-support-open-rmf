"""Load fleet identity, MQTT settings, and task categories from the adapter configuration."""

import json
import os
import sys
import uuid
from dataclasses import dataclass
from pathlib import Path

import yaml
from PySide6.QtCore import QObject, Property

ADAPTER_PACKAGE = "vda5050_fleet_adapter_full_control"

# Fallback values used when no config file is available.
_DEFAULT_INTERFACE = "uagv"
_DEFAULT_HOST = "localhost"
_DEFAULT_PORT = 1883
_DEFAULT_ADAPTER_NODE = "vda5050_fleet_adapter_full_control"
# Try the fleet-specific config before the shared config.
_DEFAULT_CONFIG_FILENAMES = ("config_tb3.yaml", "config.yaml")

# Map RMF task capabilities to dispatch categories.
_CAPABILITY_TO_CATEGORY = {
    "loop": "patrol",
    "patrol": "patrol",
    "delivery": "delivery",
    "clean": "clean",
}

# Show only task categories the UI can create.
_UI_SUPPORTED_CATEGORIES = ("patrol", "delivery")


@dataclass(frozen=True)
class RobotIdentity:
    """A robot's VDA5050 address, adapter node, and RMF fleet."""

    name: str
    manufacturer: str
    serial: str
    interface_name: str
    adapter_node: str
    fleet_name: str

    def topic(self, leaf: str) -> str:
        """Full VDA5050 topic: '<interface>/v2/<manufacturer>/<serial>/<leaf>'."""
        return f"{self.interface_name}/v2/{self.manufacturer}/{self.serial}/{leaf}"

    @property
    def topic_prefix(self) -> str:
        return f"{self.interface_name}/v2/{self.manufacturer}/{self.serial}/"


@dataclass(frozen=True)
class FleetConfig:
    fleet_name: str
    fleet_names: tuple[str, ...]  # every real RMF fleet name loaded (fleet_name may be a "a + b" display join)
    interface_name: str
    broker_host: str
    broker_port: int
    username: str | None
    password: str | None
    robots: tuple[RobotIdentity, ...]
    task_categories: tuple[str, ...]
    nav_graph: Path | None
    websocket_uri: str | None  # None disables task events
    source: str  # Config source(s) for startup logs

    def robot_for_topic(self, topic: str) -> RobotIdentity | None:
        for r in self.robots:
            if topic.startswith(r.topic_prefix):
                return r
        return None


def _default_adapter_sources() -> list[tuple[Path, str, str]]:
    """List config candidates for a single fleet in priority order."""
    node_name = os.environ.get("EIU_FLEET_ADAPTER_NODE") or _DEFAULT_ADAPTER_NODE

    explicit = os.environ.get("EIU_FLEET_CONFIG")
    if explicit:
        return [(Path(explicit), node_name, "$EIU_FLEET_CONFIG")]

    out: list[tuple[Path, str, str]] = []
    src_root = Path(__file__).resolve().parents[2]
    for filename in _DEFAULT_CONFIG_FILENAMES:
        out.append((src_root / ADAPTER_PACKAGE / "config" / filename, node_name,
                    f"{ADAPTER_PACKAGE} source tree"))

    try:
        from ament_index_python.packages import get_package_share_directory
        share = Path(get_package_share_directory(ADAPTER_PACKAGE))
        for filename in _DEFAULT_CONFIG_FILENAMES:
            out.append((share / "config" / filename, node_name,
                        f"{ADAPTER_PACKAGE} share directory"))
    except Exception:
        pass

    return out


def _adapter_fleets() -> list[list[tuple[Path, str, str]]]:
    """Group config candidates by fleet, using EIU_FLEET_ADAPTERS when set."""
    multi = os.environ.get("EIU_FLEET_ADAPTERS")
    if not multi:
        return [_default_adapter_sources()]

    fleets: list[list[tuple[Path, str, str]]] = []
    for entry in multi.split(","):
        entry = entry.strip()
        if not entry:
            continue
        path_str, sep, node_name = entry.partition("=")
        if not sep or not path_str.strip() or not node_name.strip():
            print(f"[CFG] skipping malformed EIU_FLEET_ADAPTERS entry {entry!r} "
                  f"(expected path=node_name)", file=sys.stderr)
            continue
        fleets.append([(Path(path_str.strip()), node_name.strip(), "$EIU_FLEET_ADAPTERS")])
    return fleets


def _parse_robots(vda: dict, interface: str, adapter_node: str, fleet_name: str) -> tuple[RobotIdentity, ...]:
    robots = vda.get("robots") or {}
    out = []
    for name, rc in robots.items():
        rc = rc or {}
        out.append(RobotIdentity(
            name=str(name),
            manufacturer=str(rc.get("manufacturer", "unknown")),
            serial=str(rc.get("serial", name)),
            interface_name=interface,
            adapter_node=adapter_node,
            fleet_name=fleet_name,
        ))
    return tuple(out)


def _parse_categories(rmf: dict) -> tuple[str, ...]:
    caps = rmf.get("task_capabilities") or {}
    advertised = []
    for key, enabled in caps.items():
        if not enabled:
            continue
        category = _CAPABILITY_TO_CATEGORY.get(str(key).lower())
        if category and category not in advertised:
            advertised.append(category)

    usable = [c for c in advertised if c in _UI_SUPPORTED_CATEGORIES]
    skipped = [c for c in advertised if c not in _UI_SUPPORTED_CATEGORIES]
    if skipped:
        print(f"[CFG] fleet advertises {skipped} but the UI cannot build those "
              f"requests yet — not offering them in the task dialog")
    return tuple(usable) or ("patrol",)


def _load_one_fleet(candidates: list[tuple[Path, str, str]]) -> FleetConfig | None:
    """Parse the first available fleet config candidate."""
    data, source, path, node_name = None, None, None, None
    for candidate, node_name_c, label in candidates:
        try:
            if candidate.is_file():
                data = yaml.safe_load(candidate.read_text()) or {}
                source, path, node_name = f"{label} ({candidate})", candidate, node_name_c
                break
        except Exception as exc:
            print(f"[CFG] cannot read {candidate}: {exc}", file=sys.stderr)

    if data is None:
        return None

    vda = data.get("vda5050") or {}
    rmf = data.get("rmf_fleet") or {}
    mqtt = vda.get("mqtt") or {}

    interface = str(vda.get("interface_name") or _DEFAULT_INTERFACE)
    host = os.environ.get("EIU_MQTT_HOST") or str(mqtt.get("host") or _DEFAULT_HOST)
    try:
        port = int(os.environ.get("EIU_MQTT_PORT") or mqtt.get("port") or _DEFAULT_PORT)
    except (TypeError, ValueError):
        port = _DEFAULT_PORT

    # Prefer the navigation graph loaded by the adapter.
    nav_graph = None
    if path is not None:
        candidate_graph = path.parent.parent / "maps" / "nav_graph.yaml"
        if candidate_graph.is_file():
            nav_graph = candidate_graph

    fleet_name = str(rmf.get("name") or "fleet")
    robots = _parse_robots(vda, interface, node_name, fleet_name)
    if not robots:
        print(f"[CFG] no robots declared under vda5050.robots in {source} — this "
              f"fleet will show no VDA5050 telemetry.", file=sys.stderr)

    websocket_uri = vda.get("ui_websocket_uri") or None

    return FleetConfig(
        fleet_name=fleet_name,
        fleet_names=(fleet_name,),
        interface_name=interface,
        broker_host=host,
        broker_port=port,
        username=mqtt.get("username") or None,
        password=mqtt.get("password") or None,
        robots=robots,
        task_categories=_parse_categories(rmf),
        nav_graph=nav_graph,
        websocket_uri=websocket_uri,
        source=source,
    )


def _merge_fleets(fleets: list[FleetConfig]) -> FleetConfig:
    """Merge fleets; share broker and map settings, combine robots and tasks."""
    first = fleets[0]

    robots: list[RobotIdentity] = []
    task_categories: list[str] = []
    nav_graph = None
    websocket_uri = None
    for f in fleets:
        robots.extend(f.robots)
        for c in f.task_categories:
            if c not in task_categories:
                task_categories.append(c)
        if nav_graph is None:
            nav_graph = f.nav_graph
        if websocket_uri is None:
            websocket_uri = f.websocket_uri

    return FleetConfig(
        fleet_name=" + ".join(f.fleet_name for f in fleets),
        fleet_names=tuple(f.fleet_name for f in fleets),
        interface_name=first.interface_name,
        broker_host=first.broker_host,
        broker_port=first.broker_port,
        username=first.username,
        password=first.password,
        robots=tuple(robots),
        task_categories=tuple(task_categories) or ("patrol",),
        nav_graph=nav_graph,
        websocket_uri=websocket_uri,
        source="; ".join(f.source for f in fleets),
    )


def load_fleet_config() -> FleetConfig:
    """Read every configured fleet adapter's config, falling back to
    built-in defaults (with a warning) if none can be found."""
    fleets = []
    for candidates in _adapter_fleets():
        fleet = _load_one_fleet(candidates)
        if fleet is not None:
            fleets.append(fleet)

    if not fleets:
        print("[CFG] no adapter config found — falling back to defaults. "
              "Set EIU_FLEET_CONFIG (single fleet) or EIU_FLEET_ADAPTERS "
              "(multiple fleets, 'path=node_name,...') to point at the "
              "fleet adapter(s)' config file(s).", file=sys.stderr)
        fleets = [FleetConfig(
            fleet_name="fleet", fleet_names=("fleet",), interface_name=_DEFAULT_INTERFACE,
            broker_host=_DEFAULT_HOST, broker_port=_DEFAULT_PORT,
            username=None, password=None, robots=(), task_categories=("patrol",),
            nav_graph=None, websocket_uri=None, source="built-in defaults")]

    return _merge_fleets(fleets)


def client_id(prefix: str = "eiu_fleet_ui") -> str:
    """Generate a unique MQTT client ID for a caller."""
    return f"{prefix}-{uuid.uuid4().hex[:8]}"


class FleetSettings(QObject):
    """Exposes the fleet config to QML as the context property `cfg`."""

    def __init__(self, config: FleetConfig, parent=None):
        super().__init__(parent)
        self._c = config

    @Property(str, constant=True)
    def fleetName(self):
        return self._c.fleet_name

    @Property(str, constant=True)
    def brokerLabel(self):
        return f"{self._c.broker_host}:{self._c.broker_port}"

    @Property(list, constant=True)
    def taskCategories(self):
        return list(self._c.task_categories)

    @Property(str, constant=True)
    def robotNamesJson(self):
        return json.dumps([r.name for r in self._c.robots])

    @Property(bool, constant=True)
    def websocketEnabled(self):
        return self._c.websocket_uri is not None

    @Property(str, constant=True)
    def primaryRobot(self):
        return self._c.robots[0].name if self._c.robots else ""
