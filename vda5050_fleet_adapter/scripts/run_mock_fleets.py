#!/usr/bin/env python3
"""
run_mock_fleets.py — start one mock VDA5050 robot per robot in fleet config files.

Each robot takes its MQTT identity from the `vda5050:` block and starts at its
charger waypoint from the nav graph, so the mocks match the fleet adapters
started with the same config files.

Usage:
  python3 run_mock_fleets.py config_amr.yaml config_tb3.yaml
  python3 run_mock_fleets.py config_tb3.yaml --robots tb3_1 --host localhost --port 1883
"""
import argparse
import os
import subprocess
import sys

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))


def find_nav_graph(explicit=None):
    """Nav graph next to the sources, or in the installed share directory."""
    candidates = [explicit] if explicit else [
        os.path.join(HERE, "..", "maps", "nav_graph.yaml"),
        os.path.join(HERE, "..", "..", "..", "share", "vda5050_fleet_adapter", "maps",
                     "nav_graph.yaml"),
    ]
    for path in candidates:
        if path and os.path.isfile(path):
            return os.path.abspath(path)
    raise FileNotFoundError("nav_graph.yaml not found; pass --nav-graph")


def load_chargers(nav_graph_path):
    """Return (map name, {waypoint name: (x, y)}) for the nav graph."""
    with open(nav_graph_path) as f:
        graph = yaml.safe_load(f)
    map_name, level = next(iter(graph["levels"].items()))
    points = {}
    for x, y, attrs in level["vertices"]:
        if attrs.get("name"):
            points[attrs["name"]] = (x, y)
    return map_name, points


def load_robots(config_path, nav_graph_path):
    """The robots of one fleet config, with identity, broker and start pose."""
    with open(config_path) as f:
        config = yaml.safe_load(f)
    map_name, points = load_chargers(nav_graph_path)

    fleet = config["rmf_fleet"]
    vda = config["vda5050"]
    mqtt = vda.get("mqtt", {})
    robots = []
    for name, identity in vda["robots"].items():
        charger = fleet["robots"][name]["charger"]
        x, y = points[charger]
        robots.append({
            "fleet": fleet["name"],
            "name": name,
            "interface": vda["interface_name"],
            "manufacturer": identity["manufacturer"],
            "serial": str(identity["serial"]),
            "host": mqtt.get("host", "localhost"),
            "port": int(mqtt.get("port", 1883)),
            "map": map_name,
            "start_node": charger,
            "x": x,
            "y": y,
        })
    return robots


LOCAL_HOSTS = ("localhost", "127.0.0.1", "::1")


def check_local_broker(host):
    """Mocks reuse real robot identities, so refuse a non-local broker by default."""
    if host not in LOCAL_HOSTS:
        raise SystemExit(
            f"refusing to publish mock robots to '{host}': their identities match real "
            "robots. Use --host localhost (with a local broker) or pass --allow-remote.")


def spawn_mock(robot, host=None, port=None, step_time=0.2):
    """Start mock_mqtt_robot.py for one robot; returns the process."""
    return subprocess.Popen([
        sys.executable, os.path.join(HERE, "mock_mqtt_robot.py"),
        "--host", host or robot["host"], "--port", str(port or robot["port"]),
        "--interface", robot["interface"], "--manufacturer", robot["manufacturer"],
        "--serial", robot["serial"], "--map", robot["map"],
        "--start-node", robot["start_node"],
        "--x", str(robot["x"]), "--y", str(robot["y"]), "--theta", "0",
        "--step-time", str(step_time),
    ])


def main() -> int:
    p = argparse.ArgumentParser(description="Run mock robots for fleet config files.")
    p.add_argument("configs", nargs="+", help="fleet config YAML files")
    p.add_argument("--nav-graph", help="nav graph YAML (default: the package's)")
    p.add_argument("--robots", help="comma-separated robot names to start (default: all)")
    p.add_argument("--host", help="override the broker host from the configs")
    p.add_argument("--port", type=int, help="override the broker port from the configs")
    p.add_argument("--allow-remote", action="store_true",
                   help="allow a broker that is not localhost")
    p.add_argument("--step-time", type=float, default=0.2,
                   help="seconds per simulated motion step")
    args = p.parse_args()

    graph = find_nav_graph(args.nav_graph)
    wanted = set(args.robots.split(",")) if args.robots else None

    procs = []
    for config in args.configs:
        for robot in load_robots(config, graph):
            if wanted and robot["name"] not in wanted:
                continue
            if not args.allow_remote:
                check_local_broker(args.host or robot["host"])
            print(f"[mock] {robot['fleet']}/{robot['name']} -> "
                  f"{robot['interface']}/v2/{robot['manufacturer']}/{robot['serial']} "
                  f"at {robot['start_node']}", flush=True)
            procs.append(spawn_mock(robot, args.host, args.port, args.step_time))

    if not procs:
        print("[mock] no robots selected")
        return 1
    try:
        for proc in procs:
            proc.wait()
    except KeyboardInterrupt:
        pass
    finally:
        for proc in procs:
            proc.terminate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
