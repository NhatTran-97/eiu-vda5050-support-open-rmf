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
    """One robot's VDA5050 address on the broker."""

    name: str
    manufacturer: str
    serial: str
    interface_name: str

    def topic(self, leaf: str) -> str:
        """Full VDA5050 topic: '<interface>/v2/<manufacturer>/<serial>/<leaf>'."""
        return f"{self.interface_name}/v2/{self.manufacturer}/{self.serial}/{leaf}"

    @property
    def topic_prefix(self) -> str:
        return f"{self.interface_name}/v2/{self.manufacturer}/{self.serial}/"


@dataclass(frozen=True)
class FleetConfig:
    fleet_name: str
    interface_name: str
    broker_host: str
    broker_port: int
    username: str | None
    password: str | None
    robots: tuple[RobotIdentity, ...]
    task_categories: tuple[str, ...]
    nav_graph: Path | None
    websocket_uri: str | None  # None disables task events
    source: str  # Config source for startup logs

    def robot_for_topic(self, topic: str) -> RobotIdentity | None:
        for r in self.robots:
            if topic.startswith(r.topic_prefix):
                return r
        return None


def _candidate_paths() -> list[tuple[Path, str]]:
    """Config file locations to try, most specific first."""
    out: list[tuple[Path, str]] = []

    explicit = os.environ.get("EIU_FLEET_CONFIG")
    if explicit:
        out.append((Path(explicit), "$EIU_FLEET_CONFIG"))

    # Find the adapter config in the workspace source tree.
    src_root = Path(__file__).resolve().parents[2]
    out.append((src_root / ADAPTER_PACKAGE / "config" / "config.yaml",
                f"{ADAPTER_PACKAGE} source tree"))

    try:
        from ament_index_python.packages import get_package_share_directory
        share = Path(get_package_share_directory(ADAPTER_PACKAGE))
        out.append((share / "config" / "config.yaml",
                    f"{ADAPTER_PACKAGE} share directory"))
    except Exception:
        pass

    return out


def _parse_robots(vda: dict, interface: str) -> tuple[RobotIdentity, ...]:
    robots = vda.get("robots") or {}
    out = []
    for name, rc in robots.items():
        rc = rc or {}
        out.append(RobotIdentity(
            name=str(name),
            manufacturer=str(rc.get("manufacturer", "unknown")),
            serial=str(rc.get("serial", name)),
            interface_name=interface,
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


def load_fleet_config() -> FleetConfig:
    """Read the adapter config, falling back to defaults with a warning."""
    data, source, path = None, "built-in defaults", None
    for candidate, label in _candidate_paths():
        try:
            if candidate.is_file():
                data = yaml.safe_load(candidate.read_text()) or {}
                source, path = f"{label} ({candidate})", candidate
                break
        except Exception as exc:
            print(f"[CFG] cannot read {candidate}: {exc}", file=sys.stderr)

    if data is None:
        print("[CFG] no adapter config found — falling back to defaults. "
              "Set EIU_FLEET_CONFIG to point at vda5050_fleet_adapter's "
              "config.yaml.", file=sys.stderr)
        data = {}

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
        candidate = path.parent.parent / "maps" / "nav_graph.yaml"
        if candidate.is_file():
            nav_graph = candidate

    robots = _parse_robots(vda, interface)
    if not robots:
        print("[CFG] no robots declared under vda5050.robots — the UI will show "
              "no VDA5050 telemetry. Check the config path above.", file=sys.stderr)

    websocket_uri = vda.get("ui_websocket_uri") or None

    return FleetConfig(
        fleet_name=str(rmf.get("name") or "fleet"),
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
