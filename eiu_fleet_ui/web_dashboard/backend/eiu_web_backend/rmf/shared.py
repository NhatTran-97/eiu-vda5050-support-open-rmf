"""Qt-free modules of eiu_fleet_ui reused by the backend, from the installed package or the source tree."""

import sys
from pathlib import Path

try:
    from eiu_fleet_ui import metrics_model, task_state  # noqa: F401
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[4]))
    from eiu_fleet_ui import metrics_model, task_state  # noqa: F401

STATE_LABEL = {
    "queued": "queued", "selected": "queued", "dispatching": "queued", "uninitialized": "queued",
    "standby": "queued", "underway": "underway", "delayed": "underway", "blocked": "underway",
    "completed": "completed", "failed": "failed", "error": "failed", "killed": "failed",
    "cancelled": "cancelled", "canceled": "cancelled", "skipped": "cancelled",
}
