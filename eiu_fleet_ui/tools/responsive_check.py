#!/usr/bin/env python3
"""Check UI clipping and waypoint overlap across screen sizes."""
import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

# Render offscreen to measure windows at the requested size.
_SCREEN_CFG = Path(tempfile.gettempdir()) / "eiu_offscreen_screen.json"
_SCREEN_CFG.write_text(json.dumps({"screens": [{
    "name": "virtual-8k", "x": 0, "y": 0, "width": 7680, "height": 4320,
    "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1}]}))
os.environ["QT_QPA_PLATFORM"] = f"offscreen:configfile={_SCREEN_CFG}"
os.environ["QT_QUICK_BACKEND"] = "software"
# Use the logical dimensions seen by QML.
os.environ.pop("QT_SCALE_FACTOR", None)

PKG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PKG_ROOT))

from PySide6.QtCore import QEventLoop, QRectF, QTimer  # noqa: E402
from PySide6.QtGui import QWindow  # noqa: E402
# Import QQuickWindow to enable capturing the QML window.
from PySide6.QtQuick import QQuickWindow  # noqa: E402,F401
from PySide6.QtWidgets import QApplication  # noqa: E402

# Logical screen sizes used for layout checks.
SIZES = [
    ("min-window",     1120,  700),
    ("laptop-hd",      1366,  768),
    ("app-default",    1440,  900),
    ("sxga-5x4",       1280, 1024),
    ("fhd-1080p",      1920, 1080),
    ("laptop-16x10",   2560, 1600),
    ("qhd-1440p",      2560, 1440),
    ("ultrawide-21x9", 3440, 1440),
    ("4k-tv-60in",     3840, 2160),
    ("superwide-32x9", 5120, 1440),
]

CLIP_TOLERANCE = 1.5  # Pixel tolerance for rounding


def settle(app, ms=600):
    loop = QEventLoop()
    QTimer.singleShot(ms, loop.quit)
    loop.exec()
    app.processEvents()


def descendants(item):
    for child in item.childItems():
        yield child
        yield from descendants(child)


def effectively_visible(item):
    while item is not None:
        if not item.isVisible() or item.opacity() <= 0.01:
            return False
        item = item.parentItem()
    return True


def scene_rect(item):
    return item.mapRectToScene(QRectF(0, 0, item.width(), item.height()))


def clip_ancestor(item):
    """Nearest clipping ancestor, or None. A Flickable clips on purpose (scrolling)."""
    parent = item.parentItem()
    while parent is not None:
        if parent.inherits("QQuickFlickable"):
            return "scrollable"
        if parent.clip():
            return parent
        parent = parent.parentItem()
    return None


def contains(outer, inner):
    t = CLIP_TOLERANCE
    return (inner.left() >= outer.left() - t and inner.top() >= outer.top() - t and
            inner.right() <= outer.right() + t and inner.bottom() <= outer.bottom() + t)


def check(window, waypoint_names):
    issues = []
    win_rect = QRectF(0, 0, window.width(), window.height())
    labels = []

    for item in descendants(window.contentItem()):
        if not item.inherits("QQuickText") or not effectively_visible(item):
            continue
        text = item.property("text") or ""
        if not text.strip() or item.width() <= 0 or item.height() <= 0:
            continue

        rect = scene_rect(item)
        ancestor = clip_ancestor(item)
        if ancestor == "scrollable":
            continue
        bound = scene_rect(ancestor) if ancestor is not None else win_rect
        if not contains(bound, rect):
            issues.append(f"CLIPPED  '{text[:40]}' at y={rect.top():.0f}..{rect.bottom():.0f}"
                          f" (box y={bound.top():.0f}..{bound.bottom():.0f})")

        if text in waypoint_names:
            labels.append((text, rect))

    for i in range(len(labels)):
        for j in range(i + 1, len(labels)):
            (a, ra), (b, rb) = labels[i], labels[j]
            if ra.intersects(rb):
                issues.append(f"OVERLAP  '{a}' and '{b}'")
    return issues


def build_engine():
    """The app's own build_engine, so the harness can't drift from the real wiring."""
    from eiu_fleet_ui.main import build_engine as build_app_engine

    app = QApplication(sys.argv[:1])
    app.setOrganizationName("EIU")
    app.setApplicationName("EIU Fleet UI")
    engine, backends = build_app_engine(app)
    if not engine.rootObjects():
        sys.exit("main.qml failed to load")
    backends.ros.set_waypoints(backends.map_prov.waypoints())

    names = {w["name"] for w in backends.map_prov.waypoints() if w.get("name")}
    return app, engine, backends, names


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", help="run only sizes whose name contains this")
    parser.add_argument("--out", default="/tmp/eiu_responsive", help="screenshot directory")
    args = parser.parse_args()

    app, engine, _backends, waypoint_names = build_engine()
    window = engine.rootObjects()[0]
    # A maximized window ignores resize(); each case sets its own size.
    window.setVisibility(QWindow.Visibility.Windowed)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    failed = 0
    for name, w, h in SIZES:
        if args.only and args.only not in name:
            continue
        window.setMinimumWidth(min(w, window.minimumWidth()))
        window.setMinimumHeight(min(h, window.minimumHeight()))
        window.resize(w, h)
        window.show()
        settle(app)

        shot = out / f"{name}_{w}x{h}.png"
        image = window.grabWindow()
        image.save(str(shot))

        issues = check(window, waypoint_names)
        if (image.width(), image.height()) != (w, h):
            issues.insert(0, f"SIZE     rendered {image.width()}x{image.height()}, not {w}x{h}"
                             " -- this case did not test what it claims")
        status = "OK  " if not issues else "FAIL"
        print(f"[{status}] {name:<15} {w}x{h}  {shot}")
        for issue in issues:
            print(f"         {issue}")
        failed += bool(issues)

    print(f"\n{failed} size(s) with issues")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
