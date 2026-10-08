import pytest

from eiu_web_backend.settings import SettingsError, load
from eiu_web_backend.site import image_info


def test_defaults_and_env_override(tmp_path):
    s = load(environ={"EIU_WEB_PORT": "9001", "EIU_WEB_DATABASE": str(tmp_path / "x.db")})
    assert s.get("server", "port") == 9001
    assert s.path("server", "database") == tmp_path / "x.db"


def test_unknown_key_and_bad_type_are_rejected(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text("server:\n  prot: 1\n")
    with pytest.raises(SettingsError, match="server.prot"):
        load(bad, environ={})
    bad.write_text("delivery:\n  max_open: many\n")
    with pytest.raises(SettingsError, match="delivery.max_open"):
        load(bad, environ={})


def test_catalog_matches_nav_graph(site):
    assert set(site.levels) == {"tb3_world"}
    level = site.levels["tb3_world"]
    assert level.width_px == 639 and level.height_px == 191
    room = site.location("room_204")
    assert room["waypoint"] == "Patrol_F3" and room["dispenser"] == "mock_dispenser_1"
    assert site.level_of_rmf("L1") == "tb3_world"
    assert any(t["kind"] == "patrol" for t in site.templates)


def test_image_info_reads_png_header():
    png = b"\x89PNG\r\n\x1a\n" + b"\x00\x00\x00\rIHDR" + (640).to_bytes(4, "big") + (480).to_bytes(4, "big")
    assert image_info(png) == ("image/png", 640, 480)


def test_route_on_nav_graph(site):
    level = site.levels["tb3_world"]
    route = level.route(10.4, -8.2, "Patrol_F3")
    assert route[0] == (level.vertices[10]["x"], level.vertices[10]["y"])
    assert route[-1] == (28.775689703896944, -11.317981385983169)
    assert level.route(10.4, -8.2, "missing") is None
