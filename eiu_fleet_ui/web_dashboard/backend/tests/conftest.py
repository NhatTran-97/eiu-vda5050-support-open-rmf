import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from eiu_web_backend import security  # noqa: E402
from eiu_web_backend.db import Database, User  # noqa: E402
from eiu_web_backend.settings import load  # noqa: E402
from eiu_web_backend.access import Access  # noqa: E402
from eiu_web_backend.services import Registry  # noqa: E402
from eiu_web_backend.site import Site  # noqa: E402


class FakeBridge:
    """Stands in for GatewayClient: records sent requests, replays queued events and robots."""

    available = True

    def __init__(self):
        self.sent = []
        self.events = []
        self.robots = {}
        self.workcells = {}
        self.commands = []
        self.ops = {"adapters": {}, "controls": {}, "metrics": {}, "lanes": {}, "registry": {}, "discovery": {},
                    "nav_graph": None}
        self.gateway = {"ros_domain_id": "7"}
        # Called with (type, id, body); returns (result, final) to deliver to the waiter, or None for no answer.
        self.responder = None
        self.waiter = None

    def command(self, cmd_type, cmd_id, body):
        self.commands.append((cmd_type, cmd_id, body))
        if self.responder and self.waiter:
            answer = self.responder(cmd_type, cmd_id, body)
            if answer is not None:
                self.waiter.resolve(cmd_id, answer[0], final=answer[1])
        return True

    def send(self, request_id, envelope):
        self.sent.append((request_id, envelope))
        return True

    def task_event(self, data):
        self.events.append(("task_state", data))

    def drain(self):
        events, self.events = self.events, []
        return events

    def snapshot(self, _offline_s):
        return dict(self.robots), True, dict(self.workcells)


@pytest.fixture
def settings(tmp_path):
    return load(environ={"EIU_WEB_DATABASE": str(tmp_path / "test.sqlite3")})


@pytest.fixture
def site(settings):
    return Site.load(settings)


@pytest.fixture
def db(settings):
    database = Database.sqlite(settings.path("server", "database"))
    with database.session() as s:
        for uid, email, role, services in (("u1", "student@eiu.edu.vn", "operator", ["delivery", "patrol"]),
                                           ("u2", "other@eiu.edu.vn", "operator", ["delivery", "patrol"]),
                                           ("u3", "patrol@eiu.edu.vn", "operator", ["patrol"]),
                                           ("a1", "admin@eiu.edu.vn", "admin", [])):
            s.add(User(id=uid, email=email, full_name=email.split("@")[0], role=role, allowed_services=services,
                       permissions=list(security.DEFAULT_OPERATOR), password_hash=security.hash_password("password123")))
        s.commit()
    return database


@pytest.fixture
def bridge():
    return FakeBridge()


@pytest.fixture
def registry(settings, site):
    return Registry.load(settings.path("site", "services"), site)


@pytest.fixture
def access(registry, site, db):
    return Access(registry, site, db)
