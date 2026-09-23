"""The dashboard's own tunables: config/ui_settings.yaml, overridden by EIU_* environment variables.

Every key has a default, so the file may leave any of them out. A value of the wrong type or out
of range, and a key the dashboard does not know, is reported and the default is used instead.
"""

import math
import os
import sys
from dataclasses import dataclass
from pathlib import Path

import yaml

from .metrics_model import DEFAULT_CAPACITY, DEFAULT_GRACE_S, DEFAULT_SILENT_FACTOR
from .vda5050.graph import DEFAULT_OFF_GRAPH_HOLD_S, DEFAULT_OFF_GRAPH_LIMIT_M

CONFIG_ENV = "EIU_UI_CONFIG"
CONFIG_FILE = "ui_settings.yaml"


@dataclass(frozen=True)
class Setting:
    default: object
    minimum: float | None = None
    maximum: float | None = None
    env: str | None = None
    integer: bool = False


SETTINGS = {
    "dashboard.refresh_period_s":      Setting(0.2, 0.02, 5.0, env="EIU_UI_REFRESH_PERIOD"),
    "rmf.offline_after_s":             Setting(5.0, 0.5, 600.0, env="EIU_RMF_OFFLINE_AFTER"),
    "rmf.dispatch_timeout_s":          Setting(15.0, 1.0, 600.0, env="EIU_DISPATCH_TIMEOUT"),
    "rmf.default_level":               Setting("L1"),
    "tasks.history":                   Setting(50, 1, 10000, env="EIU_TASK_HISTORY", integer=True),
    "tasks.cache_write_delay_s":       Setting(1.0, 0.01, 60.0, env="EIU_TASK_CACHE_DELAY"),
    "commands.timeout_s":              Setting(10.0, 1.0, 600.0, env="EIU_COMMAND_TIMEOUT"),
    "registration.reply_timeout_s":    Setting(10.0, 1.0, 600.0, env="EIU_REGISTRATION_TIMEOUT"),
    "vda5050.state_stale_after_s":     Setting(5.0, 0.5, 600.0, env="EIU_STATE_STALE_AFTER"),
    "vda5050.traffic_log_size":        Setting(200, 10, 100000, integer=True),
    "vda5050.off_graph_limit_m":       Setting(DEFAULT_OFF_GRAPH_LIMIT_M, 0.05, 100.0),
    "vda5050.off_graph_hold_s":        Setting(DEFAULT_OFF_GRAPH_HOLD_S, 0.0, 600.0),
    "adapter_metrics.history_samples": Setting(DEFAULT_CAPACITY, 2, 100000, env="EIU_METRICS_HISTORY", integer=True),
    "adapter_metrics.silent_factor":   Setting(DEFAULT_SILENT_FACTOR, 1.0, 100.0, env="EIU_METRICS_SILENT_FACTOR"),
    "adapter_metrics.grace_s":         Setting(DEFAULT_GRACE_S, 0.0, 600.0, env="EIU_ADAPTER_GRACE_S"),
    "operator.low_battery_percent":    Setting(20.0, 0.0, 100.0),
    "operator.medium_battery_percent": Setting(50.0, 0.0, 100.0),
    "map.pick_radius_m":               Setting(0.6, 0.05, 20.0),
    "map.lane_pick_radius_m":          Setting(0.35, 0.05, 20.0),
    "map.min_zoom":                    Setting(0.4, 0.05, 1.0),
    "map.max_zoom":                    Setting(8.0, 1.0, 100.0),
}

_file_values = None


def config_path() -> Path | None:
    """EIU_UI_CONFIG, else config/ui_settings.yaml from the source tree or the installed package."""
    explicit = os.environ.get(CONFIG_ENV)
    if explicit:
        return Path(explicit)
    source = Path(__file__).resolve().parents[1] / "config" / CONFIG_FILE
    if source.is_file():
        return source
    try:
        from ament_index_python.packages import get_package_share_directory
        return Path(get_package_share_directory("eiu_fleet_ui")) / "config" / CONFIG_FILE
    except Exception:
        return None


def _warn(message: str):
    print(f"[CFG] {message}", file=sys.stderr)


def _flatten(data, prefix="") -> dict:
    out = {}
    for key, value in (data or {}).items():
        name = f"{prefix}{key}"
        if isinstance(value, dict):
            out.update(_flatten(value, name + "."))
        else:
            out[name] = value
    return out


def _load_file() -> dict:
    path = config_path()
    if path is None or not path.is_file():
        if os.environ.get(CONFIG_ENV):
            _warn(f"{CONFIG_ENV}={os.environ[CONFIG_ENV]!r} is not a file -- using the defaults")
        return {}
    try:
        data = yaml.safe_load(path.read_text()) or {}
    except (OSError, yaml.YAMLError) as exc:
        _warn(f"cannot read {path} ({exc}) -- using the defaults")
        return {}
    if not isinstance(data, dict):
        _warn(f"{path} is not a mapping of settings -- using the defaults")
        return {}
    values = _flatten(data)
    for key in sorted(set(values) - set(SETTINGS)):
        _warn(f"{path}: unknown setting {key!r} is ignored")
    return {k: v for k, v in values.items() if k in SETTINGS}


def _checked(key: str, raw, origin: str):
    """The value if it has the setting's type and range, else None (reported)."""
    spec = SETTINGS[key]
    if isinstance(spec.default, str):
        if isinstance(raw, str) and raw.strip():
            return raw.strip()
        _warn(f"{origin}: {key} must be non-empty text -- using {spec.default!r}")
        return None
    try:
        if isinstance(raw, bool):
            raise ValueError
        value = float(raw)
    except (TypeError, ValueError):
        value = float("nan")
    if not math.isfinite(value) or (spec.integer and value != int(value)):
        _warn(f"{origin}: {key}={raw!r} is not a{'n integer' if spec.integer else ' number'} -- using {spec.default}")
        return None
    if (spec.minimum is not None and value < spec.minimum) or (spec.maximum is not None and value > spec.maximum):
        _warn(f"{origin}: {key}={raw!r} is outside {spec.minimum}..{spec.maximum} -- using {spec.default}")
        return None
    return int(value) if spec.integer else value


def get(key: str):
    """The setting's value: its environment variable, else the config file, else its default."""
    global _file_values
    spec = SETTINGS[key]
    if spec.env:
        raw = os.environ.get(spec.env, "").strip()
        if raw:
            value = _checked(key, raw, spec.env)
            if value is not None:
                return value
    if _file_values is None:
        _file_values = _load_file()
    if key in _file_values:
        value = _checked(key, _file_values[key], CONFIG_FILE)
        if value is not None:
            return value
    return spec.default


def reload():
    """Read the config file again on the next get()."""
    global _file_values
    _file_values = None
