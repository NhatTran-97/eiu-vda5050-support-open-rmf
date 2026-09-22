#!/usr/bin/env python3
"""Runs one load scenario: N simulated AGVs publish to a broker and the Connector reads them.

Start a broker first (for example mosquitto on --port), build the package with tests so that
load_probe exists, then:

  load_run.py --probe <build dir>/load_probe --robots 100 --state-hz 2 --viz-hz 4

It prints one RESULT line with the numbers of the steady part of the run (after a warm-up).
With --timeline it prints one line per sample instead, to watch a broker outage or restart.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WARMUP_S = 15


def run(args, sample_s):
    """Start the simulated AGVs and the probe; return the probe's samples, its stderr and the AGV publishers' summaries."""
    per = -(-args.robots // args.procs)
    generators = []
    first = 0
    while first < args.robots:
        count = min(per, args.robots - first)
        generators.append(subprocess.Popen(
            [sys.executable, os.path.join(HERE, "load_gen.py"), "--port", str(args.port), "--first", str(first),
             "--count", str(count), "--state-hz", str(args.state_hz), "--viz-hz", str(args.viz_hz),
             "--nodes", str(args.nodes), "--seconds", str(args.seconds + 8)],
            stdout=subprocess.PIPE, text=True))
        first += count
    time.sleep(3)
    subscriber = None
    if args.control_subscriber:
        subscriber = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "load_sub.py"), "--port", str(args.port), "--seconds",
             str(args.seconds - WARMUP_S - 2)], stdout=subprocess.PIPE, text=True)
    probe = subprocess.run([args.probe, str(args.robots), str(args.seconds), "tcp://127.0.0.1:%d" % args.port, str(sample_s)],
                           capture_output=True, text=True)
    generator_lines = []
    for generator in generators:
        out, _ = generator.communicate(timeout=60)
        generator_lines += [line for line in out.splitlines() if line.startswith("GEN")]
    if subscriber:
        out, _ = subscriber.communicate(timeout=60)
        print(out.strip(), flush=True)
    samples = [json.loads(line[7:]) for line in probe.stdout.splitlines() if line.startswith("SAMPLE ")]
    return samples, probe.stderr, generator_lines


def worst(distributions, field):
    return max((d[field] for d in distributions), default=0.0)


def mean(distributions, field):
    return sum(d[field] for d in distributions) / max(len(distributions), 1)


def summarize(samples, stderr, generator_lines, expected_rx_per_s):
    """The numbers of the samples taken after the warm-up."""
    steady = [s for s in samples if s["wall_s"] > WARMUP_S]
    if len(steady) < 2:
        return None
    first, last = steady[0], steady[-1]
    span = last["wall_s"] - first["wall_s"]

    def grew(group, key):
        return last[group][key] - first[group][key]

    def distributions(key):
        return [s["latency_us"][key] for s in steady if s["latency_us"][key]["count"]]

    state, wait, other = distributions("handle_state"), distributions("mutex_wait"), distributions("handle_other")
    transit = [s["state_transit_us"] for s in steady if s["state_transit_us"]["count"]]
    loop = [s["update_loop"]["pass_us"] for s in steady if s["update_loop"]["pass_us"]["count"]]
    received = sum(grew("rx", k) for k in ("state", "visualization", "connection", "factsheet"))
    return {
        "robots": last["robots"]["registered"],
        "online": last["robots"]["online"],
        "rx_per_s": round(received / span, 1),
        "delivered": round(received / span / expected_rx_per_s, 3),
        "cpu_cores": round((last["cpu_s"] - first["cpu_s"]) / span, 3),
        "rss_mb": round(last["rss_mb"], 1),
        "rss_growth_mb": round(last["rss_mb"] - first["rss_mb"], 1),
        "threads": last["threads"],
        "state_p50_us": round(mean(state, "p50"), 1),
        "state_p99_us_mean": round(mean(state, "p99"), 1),
        "state_max_us": round(worst(state, "max"), 1),
        "wait_p50_us": round(mean(wait, "p50"), 1),
        "wait_p99_us_worst": round(worst(wait, "p99"), 1),
        "wait_max_us": round(worst(wait, "max"), 1),
        "other_p99_us_worst": round(worst(other, "p99"), 1),
        "transit_p50_ms": round(mean(transit, "p50") / 1000.0, 2),
        "transit_p99_ms_worst": round(worst(transit, "p99") / 1000.0, 2),
        "transit_max_ms": round(worst(transit, "max") / 1000.0, 2),
        "state_age_max_s": round(max(s["robots"]["state_age_max_s"] for s in steady), 3),
        "loop_p50_us": round(mean(loop, "p50"), 1),
        "loop_p99_us_worst": round(worst(loop, "p99"), 1),
        "loop_max_us": round(worst(loop, "max"), 1),
        "overruns": grew("update_loop", "overruns"),
        "dropped": sum(grew("dropped", k) for k in first["dropped"]),
        "publisher_late_over_50ms": sum(int(line.split("late_over_50ms=")[1]) for line in generator_lines),
        "warn_lines": sum(1 for line in stderr.splitlines() if "[WARN]" in line and "[load]" in line),
        "error_lines": sum(1 for line in stderr.splitlines() if "[ERROR]" in line),
    }


def print_timeline(samples):
    print("t_s online age_max_s connects lost publish_ok publish_failed rx_state handle_state_p99_us loop_p99_us")
    for s in samples:
        print("%5.1f %4d %6.2f %3d %3d %5d %5d %7d %8.1f %8.1f" % (
            s["wall_s"], s["robots"]["online"], s["robots"]["state_age_max_s"], s["mqtt"]["connects"],
            s["mqtt"]["connections_lost"], s["published"]["ok"], s["published"]["failed"], s["rx"]["state"],
            s["latency_us"]["handle_state"]["p99"], s["update_loop"]["pass_us"]["p99"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--probe", required=True, help="path of the load_probe executable")
    parser.add_argument("--robots", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--state-hz", type=float, default=2.0, help="state messages per second per robot")
    parser.add_argument("--viz-hz", type=float, default=4.0, help="visualization messages per second per robot")
    parser.add_argument("--nodes", type=int, default=20, help="nodeStates in each state message")
    parser.add_argument("--procs", type=int, default=4, help="publisher processes")
    parser.add_argument("--port", type=int, default=18830)
    parser.add_argument("--label", default="")
    parser.add_argument("--dump", help="file to write every sample to, as JSON")
    parser.add_argument("--timeline", action="store_true", help="print each one-second sample instead of a summary")
    parser.add_argument("--control-subscriber", action="store_true",
                        help="also measure state age in a plain Python subscriber, to tell broker and publisher delay from the adapter's")
    args = parser.parse_args()

    samples, stderr, generator_lines = run(args, 1 if args.timeline else 5)
    if args.dump:
        with open(args.dump, "w") as out:
            json.dump(samples, out)
    if args.timeline:
        print_timeline(samples)
        return
    result = summarize(samples, stderr, generator_lines, args.robots * (args.state_hz + args.viz_hz))
    print("RESULT %s %s" % (args.label, json.dumps(result)))


if __name__ == "__main__":
    main()
