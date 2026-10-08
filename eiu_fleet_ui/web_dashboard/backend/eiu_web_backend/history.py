"""History of the robots: a sample of each robot's state every `history.sample_s`, kept `history.keep_days`.

The samples give battery trends and utilization: each sample counts as `sample_s` of the state it shows.
"""

import datetime
import zoneinfo

from sqlalchemy import delete, select

from .db import Database, RobotSample, now_ms
from .settings import Settings

DAY_MS = 86_400_000
PURGE_EVERY_MS = 3_600_000
# Utilization buckets of a sample.
ACTIVE_MODES = frozenset({"moving", "docking", "going_home", "cleaning"})


def bucket(mode: str, busy: bool) -> str:
    """active (working on a task or moving), charging, offline or idle."""
    if mode == "offline":
        return "offline"
    if mode == "charging":
        return "charging"
    if busy or mode in ACTIVE_MODES:
        return "active"
    return "idle"


class History:
    def __init__(self, settings: Settings, db: Database):
        self.settings = settings
        self.db = db
        self._last_sample = 0
        self._last_purge = 0

    @property
    def sample_ms(self) -> int:
        return int(self.settings.get("history", "sample_s") * 1000)

    def _tz(self):
        return zoneinfo.ZoneInfo(self.settings.get("site", "time_zone"))

    def tick(self, robots: list[dict], now: int | None = None) -> bool:
        """Store one sample per robot when a sample period has passed; True when it stored."""
        now = now or now_ms()
        if now - self._last_sample < self.sample_ms or not robots:
            return False
        self._last_sample = now
        with self.db.session() as s:
            for r in robots:
                s.add(RobotSample(at=now, robot=r["name"], fleet=r["fleet"], level=r.get("levelId", ""),
                                  battery=float(r["battery"]), mode=r["mode"], busy=bool(r.get("taskId"))))
            if now - self._last_purge >= PURGE_EVERY_MS:
                self._last_purge = now
                keep = int(self.settings.get("history", "keep_days")) * DAY_MS
                s.execute(delete(RobotSample).where(RobotSample.at < now - keep))
            s.commit()
        return True

    def battery(self, robot: str, hours: float) -> list[list]:
        """[[epoch ms, battery %], ...] of one robot over the last `hours`."""
        since = now_ms() - int(hours * 3_600_000)
        with self.db.session() as s:
            rows = s.execute(select(RobotSample.at, RobotSample.battery)
                             .where(RobotSample.robot == robot, RobotSample.at >= since).order_by(RobotSample.at)).all()
        return [[at, round(b, 1)] for at, b in rows]

    def day_keys(self, days: int) -> list[str]:
        today = datetime.datetime.now(self._tz()).date()
        return [(today - datetime.timedelta(days=d)).isoformat() for d in range(days - 1, -1, -1)]

    def day_start(self, days: int) -> int:
        start = datetime.datetime.now(self._tz()).replace(hour=0, minute=0, second=0, microsecond=0)
        return int((start - datetime.timedelta(days=days - 1)).timestamp() * 1000)

    def utilization(self, days: int, robot: str | None = None) -> list[dict]:
        """Hours per day in each bucket, for one robot or the whole fleet, oldest day first."""
        since = self.day_start(days)
        stmt = select(RobotSample.at, RobotSample.mode, RobotSample.busy).where(RobotSample.at >= since)
        if robot:
            stmt = stmt.where(RobotSample.robot == robot)
        hours = self.sample_ms / 3_600_000
        out = {k: {"day": k, "active": 0.0, "idle": 0.0, "charging": 0.0, "offline": 0.0} for k in self.day_keys(days)}
        tz = self._tz()
        with self.db.session() as s:
            for at, mode, busy in s.execute(stmt):
                key = datetime.datetime.fromtimestamp(at / 1000, tz).date().isoformat()
                if key in out:
                    out[key][bucket(mode, busy)] += hours
        return [{k: round(v, 2) if isinstance(v, float) else v for k, v in day.items()} for day in out.values()]

    def operating_hours(self, since: int) -> dict[str, float]:
        """Hours each robot was online and not charging since `since`."""
        hours = self.sample_ms / 3_600_000
        out: dict[str, float] = {}
        with self.db.session() as s:
            for robot, mode in s.execute(select(RobotSample.robot, RobotSample.mode).where(RobotSample.at >= since)):
                if mode not in ("offline", "charging"):
                    out[robot] = out.get(robot, 0.0) + hours
        return {r: round(h, 1) for r, h in out.items()}

    def charging_sessions(self, since: int, until: int) -> list[dict]:
        """Runs of consecutive charging samples of each robot: {robot, start, end, open}."""
        gap = 2 * self.sample_ms
        sessions: dict[str, list[dict]] = {}
        with self.db.session() as s:
            rows = s.execute(select(RobotSample.robot, RobotSample.at, RobotSample.mode)
                             .where(RobotSample.at >= since, RobotSample.at <= until)
                             .order_by(RobotSample.robot, RobotSample.at)).all()
        last: dict[str, int] = {}
        for robot, at, mode in rows:
            runs = sessions.setdefault(robot, [])
            if mode == "charging":
                if runs and runs[-1]["open"] and at - runs[-1]["end"] <= gap:
                    runs[-1]["end"] = at
                else:
                    runs.append({"robot": robot, "start": at, "end": at, "open": True})
            elif runs and runs[-1]["open"]:
                runs[-1]["open"] = False
            last[robot] = at
        out = []
        for robot, runs in sessions.items():
            for run in runs:
                run["open"] = run["open"] and last.get(robot) == run["end"]
                out.append(run)
        return sorted(out, key=lambda r: r["start"])
