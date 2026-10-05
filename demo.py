#!/usr/bin/env python3
"""Run Fleetlink acceptance scenarios through the C++ Python module"""

import argparse
import hashlib
from pathlib import Path
import sys
import tempfile

import fleetlink


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def summarize(world, capacity):
    machines = world.snapshot()
    inside = {}
    waiting = {}
    max_wait = 0
    for event in world.zone_events():
        if event.kind == fleetlink.ZoneEventKind.REQUEST:
            waiting.setdefault(event.machine, event.tick)
        elif event.kind == fleetlink.ZoneEventKind.ENTER:
            require(event.machine not in inside, "Repeated physical entry")
            require(event.machine in waiting, "Entry without a buffer request")
            inside[event.machine] = event.resource
            require(sum(zone == event.resource for zone in inside.values()) <= capacity,
                    f"Buffer capacity exceeded at tick {event.tick}")
            max_wait = max(max_wait, event.tick - waiting.pop(event.machine))
        elif event.kind in (fleetlink.ZoneEventKind.LEAVE, fleetlink.ZoneEventKind.REMOVE):
            require(inside.pop(event.machine, None) == event.resource,
                    "Physical exit without a matching entry")
    require(inside == {row.id: row.occupied_buffer for row in machines
                       if row.occupied_buffer is not None},
            "Zone events disagree with the final physical snapshot")
    for row in machines:
        if not row.on_route:
            waiting.pop(row.id, None)
    counters = world.channel_counters()
    return {
        "ticks": world.current_tick,
        "machines": [
            {"id": row.id, "loads": row.completed_loads, "unloads": row.completed_unloads,
             "on_route": row.on_route}
            for row in machines
        ],
        "max_wait": max_wait,
        "waiting": len(waiting),
        "max_open_wait": max((world.current_tick - tick for tick in waiting.values()), default=0),
        "channel": {name: getattr(counters, name)
                    for name in ("sent", "delivered", "lost", "duplicates")},
    }


def run_scenario(name, seed=42, capacity=1, log_path=None, log_level=fleetlink.LogLevel.INFO):
    require(name in ("S1", "S2", "S3", "S4", "S5"), "Unknown scenario")
    dirty = name in ("S2", "S3", "S4")
    with fleetlink.World(
        seed=seed, machines=10 if name == "S5" else 6,
        loss=0.2 if dirty else (0.1 if name == "S5" else 0.0),
        delay=(1, 10) if dirty else (0, 0), dup=0.05 if dirty else 0.0,
        zone_capacity=capacity, log_path=str(log_path) if log_path is not None else None,
        log_level=log_level,
    ) as world:
        for row in world.snapshot():
            world.dispatch(row.id, "load")
        before_failure = None
        if name == "S3":
            world.step(999)
            before_failure = {row.id: row for row in world.snapshot()}
            require(before_failure[3].state == fleetlink.MachineState.WAIT_PERMIT,
                    "S3 requires M3 to be waiting at tick 1000 for this seed and k")
            world.kill_at(machine_id=3, tick=1000)
            world.step(4001)
        elif name == "S4":
            world.step(799)
            previous = len(world.zone_events())
            world.set_arm_online(False)
            world.step(201)
            require(len(world.zone_events()) > previous, "Fleet did not progress without ARM")
            world.set_arm_online(True)
            world.step(100)
            require(all(row.telemetry is not None and row.telemetry.sequence > 1000
                        and row.age is not None and row.age < 100 for row in world.arm_snapshot()),
                    "ARM telemetry did not recover")
            world.step(3900)
        else:
            world.step({"S1": 2000, "S2": 5000, "S5": 100000}[name])
        result = summarize(world, capacity)
        for row in result["machines"]:
            if before_failure is not None:
                before = before_failure[row["id"]]
                if row["id"] == 3:
                    require(not row["on_route"] and row["loads"] == before.completed_loads
                            and row["unloads"] == before.completed_unloads,
                            "Failed machine continued its cycle")
                else:
                    require(row["loads"] > before.completed_loads
                            and row["unloads"] > before.completed_unloads,
                            f"M{row['id']} did not progress after failure")
            else:
                require(row["loads"] > 0 and row["unloads"] > 0,
                        f"M{row['id']} did not complete a cycle")
        result.update(scenario=name, seed=seed, capacity=capacity)
    return result


def journal_hash(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def report(result):
    print(f"{result['scenario']}: {len(result['machines'])} machines, {result['ticks']} ticks, "
          f"k={result['capacity']}, seed={result['seed']}")
    for row in result["machines"]:
        print(f"M{row['id']}: loads={row['loads']}, unloads={row['unloads']}")
    print("Channel: " + ", ".join(f"{name}={value}" for name, value in result["channel"].items()))
    print(f"Max completed wait: {result['max_wait']} ticks, still waiting: {result['waiting']}, "
          f"max open wait: {result['max_open_wait']}")
    if result["scenario"] == "S4":
        print("ARM recovery: PASS")
    if "journal_sha256" in result:
        print(f"Journal replay: PASS (SHA-256 {result['journal_sha256']})")
    print("Capacity checks: PASS\nProgress: PASS")


def positive_capacity(text):
    value = int(text)
    if not 1 <= value <= 2**32 - 1:
        raise argparse.ArgumentTypeError("k must be in 1..4294967295")
    return value


def seed_value(text):
    value = int(text)
    if not 0 <= value <= 2**64 - 1:
        raise argparse.ArgumentTypeError("seed must be in 0..18446744073709551615")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", nargs="+", choices=("S1", "S2", "S3", "S4", "S5"),
                        default=["S1", "S2", "S3", "S4"],
                        help="default: S1-S4; S5 runs 100000 ticks twice and compares journals")
    parser.add_argument("--seed", type=seed_value, default=42)
    parser.add_argument("--k", type=positive_capacity, default=1)
    parser.add_argument("--log-dir", type=Path, help="keep JSONL files in this directory")
    parser.add_argument("--log-level", choices=("TRACE", "DEBUG", "INFO", "WARN", "ERROR"),
                        default="INFO")
    args = parser.parse_args()
    level = getattr(fleetlink.LogLevel, args.log_level)
    try:
        with tempfile.TemporaryDirectory(prefix="fleetlink-") as temporary:
            directory = args.log_dir if args.log_dir is not None else Path(temporary)
            directory.mkdir(parents=True, exist_ok=True)
            for name in args.scenario:
                path = directory / f"{name}.jsonl"
                result = run_scenario(name, args.seed, args.k, path, level)
                if name == "S5":
                    replay_path = directory / "S5-replay.jsonl"
                    replay = run_scenario(name, args.seed, args.k, replay_path, level)
                    require(result == replay, "S5 replay produced a different report")
                    digest = journal_hash(path)
                    require(digest == journal_hash(replay_path), "S5 journals differ")
                    result["journal_sha256"] = digest
                report(result)
        return 0
    except (RuntimeError, ValueError, OSError, TypeError, OverflowError) as error:
        print(f"fleetlink: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
