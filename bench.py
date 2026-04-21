#!/usr/bin/env python3
"""
bench.py -- cserve diagnostic benchmark script.

Runs a series of ab tests designed to isolate where latency is coming
from, then prints a summary with interpretation.

Usage:
    python3 bench.py [--binary ./build/cserve] [--root ./www] [--port 18080]
"""

import argparse
import re
import shutil
import signal
import subprocess
import sys
import time


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

TESTS = [
    {
        "name":        "baseline (keep-alive, c=100)",
        "description": "Reproduces the original result.",
        "n": 10000, "c": 100, "keepalive": True,
    },
    {
        "name":        "no keep-alive, c=100",
        "description": "Isolates connection setup/teardown overhead.",
        "n": 5000,  "c": 100, "keepalive": False,
    },
    {
        "name":        "keep-alive, c=1 (serial)",
        "description": "One connection, no concurrency. "
                       "If still ~44ms, the delay is in the request path.",
        "n": 1000,  "c": 1,   "keepalive": True,
    },
    {
        "name":        "no keep-alive, c=1 (serial)",
        "description": "One connection per request, no concurrency. "
                       "Worst case for connection overhead.",
        "n": 500,   "c": 1,   "keepalive": False,
    },
    {
        "name":        "keep-alive, c=10",
        "description": "Mid-range concurrency.",
        "n": 5000,  "c": 10,  "keepalive": True,
    },
    {
        "name":        "keep-alive, c=500",
        "description": "High concurrency -- stresses the connection pool.",
        "n": 10000, "c": 500, "keepalive": True,
    },
]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def check_deps():
    missing = [t for t in ("ab",) if not shutil.which(t)]
    if missing:
        print(f"error: missing required tools: {', '.join(missing)}")
        print("  install with: sudo apt install apache2-utils")
        sys.exit(1)


def start_server(binary, port, root):
    proc = subprocess.Popen(
        [binary, "--port", str(port), "--root", root, "--log", "off"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(0.4)
    if proc.poll() is not None:
        print(f"error: server exited immediately (rc={proc.returncode})")
        sys.exit(1)
    return proc


def stop_server(proc):
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()


def run_ab(host, port, n, c, keepalive):
    url = f"http://{host}:{port}/"
    cmd = ["ab", "-n", str(n), "-c", str(c)]
    if keepalive:
        cmd.append("-k")
    cmd.append(url)

    result = subprocess.run(cmd, capture_output=True, text=True)
    return result.stdout + result.stderr


def parse_ab(output):
    """Extract key metrics from ab output."""
    def find(pattern, text, cast=float):
        m = re.search(pattern, text)
        return cast(m.group(1)) if m else None

    return {
        "rps":          find(r"Requests per second:\s+([\d.]+)", output),
        "mean_ms":      find(r"Time per request:\s+([\d.]+) \[ms\] \(mean\)\n", output),
        "mean_all_ms":  find(r"Time per request:\s+([\d.]+) \[ms\] \(mean, across", output),
        "failed":       find(r"Failed requests:\s+(\d+)", output, int),
        "p50":          find(r"\s+50%\s+(\d+)", output, int),
        "p99":          find(r"\s+99%\s+(\d+)", output, int),
        "p100":         find(r"\s+100%\s+(\d+)", output, int),
        "keepalive_n":  find(r"Keep-Alive requests:\s+(\d+)", output, int),
    }


def interpret(tests, results):
    """
    Compare results across tests to identify the likely source of latency.
    """
    lines = []

    ka_serial  = results.get("keep-alive, c=1 (serial)")
    noka_seria = results.get("no keep-alive, c=1 (serial)")
    baseline   = results.get("baseline (keep-alive, c=100)")

    if ka_serial and ka_serial["mean_ms"] is not None:
        if ka_serial["mean_ms"] > 20:
            lines.append(
                f"  * Serial keep-alive mean is {ka_serial['mean_ms']:.1f}ms. "
                "The delay is in the request handling path, not concurrency. "
                "Check epoll_wait timeout value in server.c -- a fixed sleep "
                "interval (e.g. 40-50ms) would produce exactly this pattern."
            )
        else:
            lines.append(
                f"  * Serial keep-alive mean is {ka_serial['mean_ms']:.1f}ms -- "
                "request path is fast. Latency is a concurrency/scheduling effect."
            )

    if ka_serial and noka_seria and \
            ka_serial["mean_ms"] is not None and noka_seria["mean_ms"] is not None:
        conn_overhead = noka_seria["mean_ms"] - ka_serial["mean_ms"]
        lines.append(
            f"  * Per-connection overhead: ~{conn_overhead:.1f}ms "
            f"(serial no-keepalive {noka_seria['mean_ms']:.1f}ms vs "
            f"serial keepalive {ka_serial['mean_ms']:.1f}ms)."
        )

    if baseline and baseline["p99"] is not None and baseline["p50"] is not None:
        spread = baseline["p99"] - baseline["p50"]
        if spread < 10:
            lines.append(
                f"  * p50={baseline['p50']}ms, p99={baseline['p99']}ms "
                f"(spread {spread}ms). Very tight distribution suggests a "
                "fixed timer interval rather than queueing noise."
            )

    if not lines:
        lines.append("  * No strong pattern detected from available data.")

    return lines


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="./build/cserve")
    parser.add_argument("--root",   default="./www")
    parser.add_argument("--port",   default=18080, type=int)
    parser.add_argument("--host",   default="127.0.0.1")
    args = parser.parse_args()

    check_deps()

    print(f"cserve diagnostic benchmark")
    print(f"  binary : {args.binary}")
    print(f"  root   : {args.root}")
    print(f"  target : {args.host}:{args.port}")
    print()

    collected = {}

    for test in TESTS:
        name = test["name"]
        print(f"[ {name} ]")
        print(f"  {test['description']}")

        proc = start_server(args.binary, args.port, args.root)
        try:
            raw = run_ab(args.host, args.port,
                         test["n"], test["c"], test["keepalive"])
        finally:
            stop_server(proc)

        m = parse_ab(raw)
        collected[name] = m

        if m["rps"] is not None:
            failed_str = "" if not m["failed"] else f"  FAILED={m['failed']}"
            print(f"  {m['rps']:.0f} req/s  "
                  f"mean={m['mean_ms']:.1f}ms  "
                  f"p50={m['p50']}ms  p99={m['p99']}ms  "
                  f"max={m['p100']}ms"
                  f"{failed_str}")
        else:
            print("  (could not parse ab output)")
        print()

        time.sleep(0.5)

    # Summary
    print("=" * 60)
    print("INTERPRETATION")
    print("=" * 60)
    for line in interpret(TESTS, collected):
        print(line)
    print()


if __name__ == "__main__":
    main()