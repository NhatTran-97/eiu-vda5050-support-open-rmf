"""Passwords (Argon2id), server-side sessions, CSRF tokens, sign-in lockout and permissions."""

import secrets
import threading
import time

from argon2 import PasswordHasher
from argon2.exceptions import InvalidHashError, VerificationError
from sqlalchemy import delete, select
from sqlalchemy.orm import Session

from .db import User, UserSession, now_ms

SESSION_COOKIE = "eiu_session"
CSRF_COOKIE = "csrf_token"
CSRF_HEADER = "X-CSRF-Token"

# Permissions an admin may grant to an operator, by group of the access editor.
OPERATOR_PERMISSIONS: dict[str, list[str]] = {
    "task": ["task.create", "task.cancel", "task.schedule", "task.pause", "task.reassign"],
    "fleet": ["fleet.view", "fleet.view_all", "fleet.assign", "fleet.control", "alerts.ack"],
    "insight": ["analytics.view", "maintenance.view"],
}
# Permissions only the admin role has.
ADMIN_PERMISSIONS = ["users.manage", "robots.manage", "locations.manage", "maintenance.manage",
                     "integrations.manage", "settings.manage", "system.diagnostics"]
GRANTABLE = [p for group in OPERATOR_PERMISSIONS.values() for p in group]
ALL_PERMISSIONS = GRANTABLE + ADMIN_PERMISSIONS
DEFAULT_OPERATOR = ["task.create", "task.cancel", "task.schedule", "fleet.view", "alerts.ack", "analytics.view",
                    "maintenance.view"]
ROLES = ("operator", "admin")

_hasher = PasswordHasher()


def hash_password(password: str) -> str:
    return _hasher.hash(password)


def verify_password(stored: str, password: str) -> bool:
    try:
        return _hasher.verify(stored, password)
    except (VerificationError, InvalidHashError):
        return False


def permissions(user: User) -> list[str]:
    """Effective permissions: every permission for an admin, the granted ones for an operator."""
    if user.role == "admin":
        return list(ALL_PERMISSIONS)
    granted = set(user.permissions or [])
    return [p for p in GRANTABLE if p in granted]


def can(user: User, permission: str) -> bool:
    return user.active and permission in permissions(user)


def allowed_services(user: User, enabled: list[str]) -> list[str]:
    """Enabled services the user may operate: all of them for an admin."""
    if user.role == "admin":
        return list(enabled)
    granted = set(user.allowed_services or [])
    return [s for s in enabled if s in granted]


def allowed_zones(user: User) -> set[str] | None:
    """Zones the user may send robots to; None means every zone (an admin, or an operator without a zone list)."""
    if user.role == "admin" or not user.allowed_zones:
        return None
    return set(user.allowed_zones)


def create_session(s: Session, user: User, hours: int) -> UserSession:
    session = UserSession(id=secrets.token_urlsafe(32), user_id=user.id, csrf=secrets.token_urlsafe(24),
                          expires_at=now_ms() + hours * 3600_000)
    s.add(session)
    return session


def session_user(s: Session, session_id: str | None) -> tuple[UserSession, User] | None:
    if not session_id:
        return None
    session = s.get(UserSession, session_id)
    if session is None or session.expires_at < now_ms():
        return None
    user = s.get(User, session.user_id)
    if user is None or not user.active:
        return None
    return session, user


def end_sessions(s: Session, user_id: str) -> None:
    s.execute(delete(UserSession).where(UserSession.user_id == user_id))


def purge_expired(s: Session) -> None:
    s.execute(delete(UserSession).where(UserSession.expires_at < now_ms()))


class LoginLimiter:
    """Locks an email for `lock_s` after `failures` wrong passwords in a row."""

    def __init__(self, failures: int, lock_s: float):
        self._failures = failures
        self._lock_s = lock_s
        self._state: dict[str, tuple[int, float]] = {}
        self._mutex = threading.Lock()

    def locked(self, key: str) -> bool:
        with self._mutex:
            count, until = self._state.get(key, (0, 0.0))
            return until > time.monotonic()

    def failed(self, key: str) -> None:
        with self._mutex:
            count, _ = self._state.get(key, (0, 0.0))
            count += 1
            until = time.monotonic() + self._lock_s if count >= self._failures else 0.0
            self._state[key] = (0 if until else count, until)

    def succeeded(self, key: str) -> None:
        with self._mutex:
            self._state.pop(key, None)


def any_user(s: Session) -> bool:
    return s.scalar(select(User.id).limit(1)) is not None
