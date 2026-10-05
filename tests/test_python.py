import concurrent.futures
import gc
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

import fleetlink

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import demo


def snapshots(world):
    return [(row.id, row.state, row.position, row.load, row.occupied_buffer,
             row.completed_loads, row.completed_unloads, row.halted)
            for row in world.snapshot()]


def dispatch_all(world):
    for row in world.snapshot():
        world.dispatch(row.id, "load")


class PythonBindingsTests(unittest.TestCase):
    def test_snapshots_are_readonly_copies_and_telemetry_is_delayed(self):
        with fleetlink.World(machines=4) as world:
            initial = world.snapshot()
            self.assertEqual([row.id for row in initial], [0, 1, 2, 3])
            self.assertTrue(all(row.telemetry is None for row in world.arm_snapshot()))
            dispatch_all(world)
            world.step(10)
            measured = world.snapshot()
            self.assertTrue(all(row.telemetry is None for row in world.arm_snapshot()))
            world.step()
            for row in world.arm_snapshot():
                self.assertEqual(row.age, 0)
                self.assertEqual(row.telemetry.sequence, 10)
                self.assertEqual(row.telemetry.load, measured[row.id].load)
                self.assertGreater(row.telemetry.channel.sent, 0)
            self.assertEqual(initial[0].state, fleetlink.MachineState.IDLE)
            with self.assertRaises(AttributeError):
                initial[0].load = 99
            self.assertEqual(world.current_tick, 11)
        self.assertEqual(initial[0].position, 0)

    def test_journal_contains_typed_records_and_filters_ticks(self):
        with fleetlink.World(log_level=fleetlink.LogLevel.TRACE) as world:
            dispatch_all(world)
            world.step(50)
            first = world.events()
            self.assertTrue(first)
            self.assertTrue(all({"tick", "node", "lvl", "ev"} <= row.keys() for row in first))
            self.assertTrue(any(row["ev"] == "msg_receive" for row in first))
            telemetry = next(row for row in first if row["ev"] == "telemetry")
            self.assertIsInstance(telemetry["connected"], bool)
            self.assertIsInstance(telemetry["load"], int)
            self.assertEqual(world.events(40), [row for row in first if row["tick"] >= 40])
            self.assertEqual(world.events(51), [])
            first[0]["node"] = "changed"
            self.assertNotEqual(world.events()[0]["node"], "changed")
            before = world.zone_events()
            world.step(50)
            self.assertGreater(len(world.zone_events()), len(before))
            self.assertTrue(all(row.tick <= 50 for row in before))

    def test_context_closes_file_on_exception_and_allows_reading_final_events(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            with self.assertRaisesRegex(ValueError, "example"):
                with fleetlink.World(log_path=str(path)) as world:
                    dispatch_all(world)
                    world.step(100)
                    raise ValueError("example")
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            self.assertEqual(world.events(), rows)
            self.assertTrue(any(row["tick"] == 100 for row in rows))
            world.close()
            with self.assertRaisesRegex(RuntimeError, "closed"):
                world.step()

    def test_destructor_drains_log_even_without_explicit_close(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            world = fleetlink.World(log_path=str(path))
            dispatch_all(world)
            world.step(100)
            del world
            gc.collect()
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            self.assertTrue(any(row["tick"] == 100 for row in rows))

    def test_invalid_arguments_become_python_exceptions(self):
        for kwargs in ({"machines": 3}, {"machines": 11}, {"zone_capacity": 0},
                       {"loss": 2}, {"delay": (10, 1)}, {"members": [1, 2, 2, 4]}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                fleetlink.World(**kwargs)
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(RuntimeError):
                fleetlink.World(log_path=str(Path(directory) / "missing" / "trace.jsonl"))
        with fleetlink.World() as world:
            with self.assertRaises(ValueError):
                world.dispatch(0, "invalid")
            with self.assertRaises(ValueError):
                world.dispatch(999, "load")
            with self.assertRaises(TypeError):
                world.step(-1)

    def test_configured_world_permutation_and_k3_preserve_trace(self):
        config = fleetlink.MachineConfig()
        config.zone_capacity = 3
        config.unloading_rate = 5
        channel = fleetlink.ChannelConfig()
        channel.loss = 0.2
        channel.delay_min = 1
        channel.delay_max = 10
        channel.dup = 0.05
        with fleetlink.World(members=[1, 7, 42, 99], config=config, channel=channel,
                             log_level=fleetlink.LogLevel.TRACE) as first, \
             fleetlink.World(members=[99, 42, 1, 7], config=config, channel=channel,
                             log_level=fleetlink.LogLevel.TRACE) as second:
            config.zone_capacity = 0
            channel.loss = 1
            dispatch_all(first)
            dispatch_all(second)
            first.step(1000)
            second.step(1000)
            self.assertEqual(snapshots(first), snapshots(second))
            self.assertEqual(first.events(), second.events())
            self.assertTrue(all(row.completed_unloads > 0 for row in first.snapshot()))

    def test_halt_origins_and_machine_failure_are_exposed(self):
        with fleetlink.World(zone_capacity=3) as world:
            dispatch_all(world)
            world.step(20)
            world.halt_all()
            world.step()
            frozen = snapshots(world)
            world.halt_all(initiator=2)
            world.step(250)
            world.resume()
            world.step(250)
            self.assertEqual(snapshots(world), frozen)
            world.resume(initiator=2)
            world.step(100)
            self.assertTrue(all(not row.halted for row in world.snapshot()))
            world.kill(machine_id=3)
            self.assertEqual(world.snapshot()[3].state, fleetlink.MachineState.FAULT)
            world.step(500)
            self.assertFalse(world.snapshot()[3].on_route)

    def test_concurrent_calls_serialize_model_access_without_holding_the_gil(self):
        with fleetlink.World() as world, fleetlink.World() as reference:
            dispatch_all(world)
            dispatch_all(reference)
            with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
                first = pool.submit(world.step, 1000)
                second = pool.submit(world.step, 1000)
                reader = pool.submit(world.snapshot)
                reader.result(timeout=30)
                first.result(timeout=30)
                second.result(timeout=30)
            reference.step(2000)
            self.assertEqual(snapshots(world), snapshots(reference))
            self.assertEqual(world.events(), reference.events())

    @unittest.skipUnless(os.environ.get("FLEETLINK_NATIVE_DEMO"), "native executable not provided")
    def test_s1_to_s4_match_native_counts_channel_and_waits(self):
        with tempfile.TemporaryDirectory() as directory:
            for scenario in ("S1", "S2", "S3", "S4"):
                with self.subTest(scenario=scenario):
                    result = demo.run_scenario(scenario, log_path=Path(directory) / "scenario.jsonl")
                    native = subprocess.run(
                        [os.environ["FLEETLINK_NATIVE_DEMO"], "--scenario", scenario],
                        check=True, capture_output=True, text=True, timeout=30,
                    ).stdout
                    counts = [(int(id_), int(loads), int(unloads)) for id_, loads, unloads in
                              re.findall(r"M(\d+): loads=(\d+), unloads=(\d+)", native)]
                    self.assertEqual(counts, [(row["id"], row["loads"], row["unloads"])
                                              for row in result["machines"]])
                    channel = re.search(r"Channel: sent=(\d+), delivered=(\d+), lost=(\d+), "
                                        r"duplicates=(\d+)", native)
                    self.assertEqual(tuple(map(int, channel.groups())),
                                     tuple(result["channel"].values()))
                    wait = re.search(r"Max completed wait: (\d+) ticks, still waiting: (\d+)", native)
                    self.assertEqual(tuple(map(int, wait.groups())),
                                     (result["max_wait"], result["waiting"]))


if __name__ == "__main__":
    unittest.main()
