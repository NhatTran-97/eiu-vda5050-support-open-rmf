"""Database tables (SQLAlchemy). SQLite now; the same models run on PostgreSQL with another URL."""

import time
from pathlib import Path

from sqlalchemy import (JSON, Boolean, ForeignKey, Integer, String, Text, create_engine, event, inspect, select, text)
from sqlalchemy.orm import DeclarativeBase, Mapped, Session, mapped_column, sessionmaker


def now_ms() -> int:
    return int(time.time() * 1000)


class Base(DeclarativeBase):
    pass


class User(Base):
    __tablename__ = "users"
    id: Mapped[str] = mapped_column(String(36), primary_key=True)
    email: Mapped[str] = mapped_column(String(254), unique=True, index=True)
    password_hash: Mapped[str] = mapped_column(Text)
    full_name: Mapped[str] = mapped_column(String(200))
    role: Mapped[str] = mapped_column(String(20))
    department: Mapped[str] = mapped_column(String(200), default="")
    # Operator access granted by an admin; an admin has every service, zone and permission.
    allowed_services: Mapped[list] = mapped_column(JSON, default=list)
    allowed_zones: Mapped[list] = mapped_column(JSON, default=list)
    permissions: Mapped[list] = mapped_column(JSON, default=list)
    locale: Mapped[str] = mapped_column(String(5), default="vi")
    prefs: Mapped[dict] = mapped_column(JSON, default=lambda: {"deliveryUpdates": True, "delays": True})
    active: Mapped[bool] = mapped_column(Boolean, default=True)
    created_at: Mapped[int] = mapped_column(Integer, default=now_ms)
    last_login_at: Mapped[int | None] = mapped_column(Integer, nullable=True)
    last_active_at: Mapped[int | None] = mapped_column(Integer, nullable=True)


class UserSession(Base):
    __tablename__ = "sessions"
    id: Mapped[str] = mapped_column(String(64), primary_key=True)
    user_id: Mapped[str] = mapped_column(ForeignKey("users.id", ondelete="CASCADE"), index=True)
    csrf: Mapped[str] = mapped_column(String(64))
    created_at: Mapped[int] = mapped_column(Integer, default=now_ms)
    expires_at: Mapped[int] = mapped_column(Integer)


class SavedLocation(Base):
    __tablename__ = "saved_locations"
    user_id: Mapped[str] = mapped_column(ForeignKey("users.id", ondelete="CASCADE"), primary_key=True)
    location_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    starred: Mapped[bool] = mapped_column(Boolean, default=False)
    position: Mapped[int] = mapped_column(Integer, default=0)


class Delivery(Base):
    """A robot task of a service and its RMF task. `kind` is the RMF category (delivery, patrol, clean);
    pickup_id and dropoff_id are its first and last place."""
    __tablename__ = "deliveries"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    requester_id: Mapped[str] = mapped_column(ForeignKey("users.id"), index=True)
    kind: Mapped[str] = mapped_column(String(16))
    service: Mapped[str] = mapped_column(String(32), default="", index=True)
    params: Mapped[dict] = mapped_column(JSON, default=dict)
    zones: Mapped[list] = mapped_column(JSON, default=list)
    priority: Mapped[str] = mapped_column(String(16), default="normal")
    repeat: Mapped[str] = mapped_column(String(16), default="none")
    # Earlier task of the same repeat series, or the task this one replaced on reassignment.
    previous_id: Mapped[int | None] = mapped_column(Integer, nullable=True)
    requested_robot: Mapped[str | None] = mapped_column(String(100), nullable=True)
    started_at: Mapped[int | None] = mapped_column(Integer, nullable=True)
    paused: Mapped[bool] = mapped_column(Boolean, default=False)
    pause_token: Mapped[str] = mapped_column(String(128), default="")
    pause_request_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    pickup_id: Mapped[str] = mapped_column(String(64))
    dropoff_id: Mapped[str] = mapped_column(String(64))
    stops: Mapped[list] = mapped_column(JSON, default=list)
    rounds: Mapped[int] = mapped_column(Integer, default=1)
    rounds_done: Mapped[int] = mapped_column(Integer, default=0)
    package_type: Mapped[str] = mapped_column(String(32), default="general")
    note: Mapped[str] = mapped_column(Text, default="")
    created_at: Mapped[int] = mapped_column(Integer, default=now_ms)
    scheduled_at: Mapped[int | None] = mapped_column(Integer, nullable=True)
    status: Mapped[str] = mapped_column(String(16), index=True)
    robot: Mapped[str | None] = mapped_column(String(100), nullable=True)
    fleet: Mapped[str | None] = mapped_column(String(100), nullable=True)
    request_id: Mapped[str] = mapped_column(String(64), index=True)
    rmf_task_id: Mapped[str | None] = mapped_column(String(128), nullable=True, index=True)
    rmf_state: Mapped[str] = mapped_column(String(16), default="queued")
    rmf_state_rank: Mapped[int] = mapped_column(Integer, default=0)
    phase: Mapped[int] = mapped_column(Integer, default=0)
    eta_at: Mapped[int | None] = mapped_column(Integer, nullable=True)
    finished_at: Mapped[int | None] = mapped_column(Integer, nullable=True)
    error: Mapped[str] = mapped_column(Text, default="")
    cancel_request_id: Mapped[str | None] = mapped_column(String(64), nullable=True)
    cancel_deadline: Mapped[int | None] = mapped_column(Integer, nullable=True)


class DeliveryEvent(Base):
    __tablename__ = "delivery_events"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    delivery_id: Mapped[int] = mapped_column(ForeignKey("deliveries.id", ondelete="CASCADE"), index=True)
    type: Mapped[str] = mapped_column(String(32))
    at: Mapped[int] = mapped_column(Integer)
    detail: Mapped[str] = mapped_column(Text, default="")


class Notification(Base):
    __tablename__ = "notifications"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    user_id: Mapped[str] = mapped_column(ForeignKey("users.id", ondelete="CASCADE"), index=True)
    type: Mapped[str] = mapped_column(String(32))
    delivery_id: Mapped[int | None] = mapped_column(Integer, nullable=True)
    params: Mapped[dict] = mapped_column(JSON, default=dict)
    created_at: Mapped[int] = mapped_column(Integer, default=now_ms)
    read_at: Mapped[int | None] = mapped_column(Integer, nullable=True)


class RobotSample(Base):
    """State of a robot every `history.sample_s`, for trends and utilization."""
    __tablename__ = "robot_samples"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    at: Mapped[int] = mapped_column(Integer, index=True)
    robot: Mapped[str] = mapped_column(String(100), index=True)
    fleet: Mapped[str] = mapped_column(String(100))
    level: Mapped[str] = mapped_column(String(100), default="")
    battery: Mapped[float] = mapped_column(default=0.0)
    mode: Mapped[str] = mapped_column(String(20))
    busy: Mapped[bool] = mapped_column(Boolean, default=False)


class Alert(Base):
    """Operations alert. A condition alert closes by itself when its condition ends; an event alert is closed
    by an operator."""
    __tablename__ = "alerts"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    key: Mapped[str] = mapped_column(String(200), index=True)
    kind: Mapped[str] = mapped_column(String(16), default="condition")
    severity: Mapped[str] = mapped_column(String(16))
    code: Mapped[str] = mapped_column(String(64))
    params: Mapped[dict] = mapped_column(JSON, default=dict)
    robot: Mapped[str | None] = mapped_column(String(100), nullable=True)
    delivery_id: Mapped[int | None] = mapped_column(Integer, nullable=True)
    opened_at: Mapped[int] = mapped_column(Integer, default=now_ms)
    updated_at: Mapped[int] = mapped_column(Integer, default=now_ms)
    acked_at: Mapped[int | None] = mapped_column(Integer, nullable=True)
    acked_by: Mapped[str | None] = mapped_column(String(36), nullable=True)
    resolved_at: Mapped[int | None] = mapped_column(Integer, nullable=True, index=True)
    resolved_by: Mapped[str | None] = mapped_column(String(36), nullable=True)


class MaintenanceItem(Base):
    """Planned or open maintenance of a robot; an in-progress window takes the robot out of service."""
    __tablename__ = "maintenance"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    robot: Mapped[str] = mapped_column(String(100), index=True)
    title: Mapped[str] = mapped_column(String(200))
    status: Mapped[str] = mapped_column(String(16), default="planned")
    due_at: Mapped[int | None] = mapped_column(Integer, nullable=True)
    window_start: Mapped[int | None] = mapped_column(Integer, nullable=True)
    window_end: Mapped[int | None] = mapped_column(Integer, nullable=True)
    note: Mapped[str] = mapped_column(Text, default="")
    created_by: Mapped[str | None] = mapped_column(String(36), nullable=True)
    created_at: Mapped[int] = mapped_column(Integer, default=now_ms)
    done_at: Mapped[int | None] = mapped_column(Integer, nullable=True)
    done_by: Mapped[str | None] = mapped_column(String(36), nullable=True)


class Toggle(Base):
    """Admin switch over the configuration: `service:<id>` or `zone:<id>` enabled or not."""
    __tablename__ = "toggles"
    key: Mapped[str] = mapped_column(String(100), primary_key=True)
    enabled: Mapped[bool] = mapped_column(Boolean)


class AuditLog(Base):
    __tablename__ = "audit_log"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    actor_id: Mapped[str | None] = mapped_column(String(36), nullable=True)
    action: Mapped[str] = mapped_column(String(64))
    target: Mapped[str] = mapped_column(String(200), default="")
    at: Mapped[int] = mapped_column(Integer, default=now_ms)
    detail: Mapped[str] = mapped_column(Text, default="")


class Database:
    def __init__(self, url: str):
        self.engine = create_engine(url, connect_args={"timeout": 15, "check_same_thread": False}
                                    if url.startswith("sqlite") else {})
        if url.startswith("sqlite"):
            @event.listens_for(self.engine, "connect")
            def _pragmas(conn, _record):
                cur = conn.cursor()
                cur.execute("PRAGMA journal_mode=WAL")
                cur.execute("PRAGMA foreign_keys=ON")
                cur.close()
        Base.metadata.create_all(self.engine)
        migrate(self.engine)
        self.session = sessionmaker(self.engine, expire_on_commit=False)

    @classmethod
    def sqlite(cls, path: Path) -> "Database":
        path.parent.mkdir(parents=True, exist_ok=True)
        return cls(f"sqlite:///{path}")

    def audit(self, s: Session, actor_id: str | None, action: str, target: str = "", detail: str = ""):
        s.add(AuditLog(actor_id=actor_id, action=action, target=target, detail=detail))


def user_by_email(s: Session, email: str) -> User | None:
    return s.scalar(select(User).where(User.email == email.strip().lower()))


# Columns added after the first release: table -> column -> SQL type and default.
ADDED_COLUMNS = {
    "users": {"department": "VARCHAR(200) DEFAULT ''", "allowed_services": "JSON DEFAULT '[]'",
              "allowed_zones": "JSON DEFAULT '[]'", "permissions": "JSON DEFAULT '[]'", "last_active_at": "INTEGER"},
    "deliveries": {"service": "VARCHAR(32) DEFAULT ''", "params": "JSON DEFAULT '{}'", "zones": "JSON DEFAULT '[]'",
                   "priority": "VARCHAR(16) DEFAULT 'normal'", "repeat": "VARCHAR(16) DEFAULT 'none'",
                   "previous_id": "INTEGER", "requested_robot": "VARCHAR(100)", "started_at": "INTEGER",
                   "paused": "BOOLEAN DEFAULT 0", "pause_token": "VARCHAR(128) DEFAULT ''",
                   "pause_request_id": "VARCHAR(64)"},
}
# Accounts of the former `user` role (students and staff) become operators of delivery and patrol.
MIGRATED_OPERATOR = {"services": ["delivery", "patrol"],
                     "permissions": ["task.create", "task.cancel", "fleet.view"]}


def migrate(engine) -> None:
    """Bring a database of an earlier release to the current tables."""
    import json

    columns = {t: {c["name"] for c in inspect(engine).get_columns(t)} for t in ADDED_COLUMNS}
    with engine.begin() as conn:
        for table, added in ADDED_COLUMNS.items():
            for name, sql in added.items():
                if name not in columns[table]:
                    conn.execute(text(f'ALTER TABLE {table} ADD COLUMN "{name}" {sql}'))
        conn.execute(text("UPDATE deliveries SET service = kind WHERE service = '' OR service IS NULL"))
        conn.execute(text("UPDATE users SET role = 'operator', allowed_services = :s, permissions = :p "
                          "WHERE role = 'user'"),
                     {"s": json.dumps(MIGRATED_OPERATOR["services"]), "p": json.dumps(MIGRATED_OPERATOR["permissions"])})
