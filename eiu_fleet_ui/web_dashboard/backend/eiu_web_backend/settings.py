"""Settings of the web backend: config/settings.yaml merged over defaults, a few keys overridable by env."""

import copy
import os
import sys
from dataclasses import dataclass
from pathlib import Path

import yaml

PACKAGE_DIR = Path(__file__).resolve().parent
DEFAULT_FILE = PACKAGE_DIR.parent / "config" / "settings.yaml"
SETTINGS_ENV = "EIU_WEB_SETTINGS"

DEFAULTS: dict = {
    "server": {"host": "127.0.0.1", "port": 8000, "database": "../data/eiu_web.sqlite3",
               "secure_cookies": False, "session_hours": 12},
    "site": {"name": "EIU Robot Services", "time_zone": "Asia/Ho_Chi_Minh", "default_locale": "vi",
             "catalog": "site.yaml", "services": "services.yaml"},
    "map": {"map_yaml": "../../../maps/map.yaml", "nav_graph": "../../../maps/nav_graph.yaml",
            "level": "", "rmf_levels": {}},
    "rmf": {"enabled": True, "requester": "eiu_web_dashboard", "dispatch_timeout_s": 15.0,
            "cancel_timeout_s": 15.0, "fleet_offline_s": 5.0, "correction_window_s": 600.0},
    "redis": {"url": "redis://127.0.0.1:6379/0", "prefix": "eiu:rmf", "cursor_key": "eiu:web:events_cursor"},
    "delivery": {"max_open": 20, "arriving_soon_s": 30.0, "pickup_handler": "mock_dispenser_1",
                 "dropoff_handler": "mock_ingestor_1", "schedule_min_lead_s": 60,
                 "schedule_max_ahead_days": 14, "note_max": 200},
    "patrol": {"max_stops": 8, "max_rounds": 10},
    "realtime": {"period_s": 0.5},
    "operations": {"command_timeout_s": 12.0, "registration_timeout_s": 15.0, "tasks_limit": 200,
                   "late_after_s": 120.0},
    "metrics": {"history_samples": 120, "silent_factor": 3.0, "grace_s": 10.0},
    "history": {"sample_s": 60.0, "keep_days": 30},
    "maintenance": {"due_soon_days": 3.0},
    "alerts": {"open_after_s": 5.0, "battery_low_pct": 20.0, "battery_critical_pct": 10.0, "task_waiting_s": 600.0,
               "failed_window_s": 86400.0, "history_limit": 200},
    "auth": {"min_password_length": 8, "lock_failures": 5, "lock_s": 60},
    "support": {"email": "", "phone": "", "hours_vi": "", "hours_en": ""},
}

# Environment variable -> settings key.
ENV_OVERRIDES = {
    "EIU_WEB_HOST": ("server", "host"),
    "EIU_WEB_PORT": ("server", "port"),
    "EIU_WEB_DATABASE": ("server", "database"),
    "EIU_WEB_NAV_GRAPH": ("map", "nav_graph"),
    "EIU_WEB_RMF": ("rmf", "enabled"),
    "EIU_WEB_REDIS_URL": ("redis", "url"),
}


class SettingsError(ValueError):
    pass


def _coerce(value, default, where: str):
    """Value converted to the type of its default; ints are accepted for floats."""
    if isinstance(default, bool):
        if isinstance(value, bool):
            return value
        if isinstance(value, str) and value.lower() in ("1", "true", "yes", "0", "false", "no"):
            return value.lower() in ("1", "true", "yes")
        raise SettingsError(f"{where}: expected true or false, got {value!r}")
    if isinstance(default, float):
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            return float(value)
        if isinstance(value, str):
            try:
                return float(value)
            except ValueError:
                pass
        raise SettingsError(f"{where}: expected a number, got {value!r}")
    if isinstance(default, int):
        if isinstance(value, int) and not isinstance(value, bool):
            return value
        if isinstance(value, str) and value.strip().lstrip("-").isdigit():
            return int(value)
        raise SettingsError(f"{where}: expected an integer, got {value!r}")
    if isinstance(default, str):
        if isinstance(value, (str, int, float)) and not isinstance(value, bool):
            return str(value)
        raise SettingsError(f"{where}: expected text, got {value!r}")
    if isinstance(default, dict):
        if isinstance(value, dict):
            return {str(k): str(v) for k, v in value.items()}
        raise SettingsError(f"{where}: expected a mapping, got {value!r}")
    return value


def _merge(defaults: dict, data: dict, prefix: str = "") -> dict:
    out = copy.deepcopy(defaults)
    for key, value in (data or {}).items():
        where = f"{prefix}{key}"
        if key not in defaults:
            raise SettingsError(f"{where}: unknown key")
        default = defaults[key]
        if isinstance(default, dict) and default and isinstance(value, dict):
            out[key] = _merge(default, value, where + ".")
        else:
            out[key] = _coerce(value, default, where)
    return out


@dataclass(frozen=True)
class Settings:
    data: dict
    base_dir: Path

    def get(self, section: str, key: str):
        return self.data[section][key]

    def path(self, section: str, key: str) -> Path:
        """A path setting, relative to the settings file's folder."""
        value = Path(os.path.expanduser(str(self.data[section][key])))
        return value if value.is_absolute() else (self.base_dir / value).resolve()


def load(path: str | Path | None = None, environ=os.environ) -> Settings:
    """Read and validate the settings; raises SettingsError on a bad key or value."""
    file = Path(path or environ.get(SETTINGS_ENV) or DEFAULT_FILE)
    data = yaml.safe_load(file.read_text()) if file.exists() else {}
    merged = _merge(DEFAULTS, data or {})
    for variable, (section, key) in ENV_OVERRIDES.items():
        if variable in environ:
            merged[section][key] = _coerce(environ[variable], DEFAULTS[section][key], variable)
    if merged["site"]["default_locale"] not in ("vi", "en"):
        raise SettingsError("site.default_locale: expected vi or en")
    for section, key in (("realtime", "period_s"), ("rmf", "dispatch_timeout_s"), ("rmf", "fleet_offline_s")):
        if merged[section][key] <= 0:
            raise SettingsError(f"{section}.{key}: must be positive")
    return Settings(merged, file.parent.resolve())


def load_or_exit(path=None) -> Settings:
    try:
        return load(path)
    except (SettingsError, OSError, yaml.YAMLError) as e:
        print(f"[settings] {e}", file=sys.stderr)
        raise SystemExit(2)
