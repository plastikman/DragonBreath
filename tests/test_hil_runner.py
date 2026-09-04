#!/usr/bin/env python3
import importlib.util
import io
import json
import pathlib
import sys
import tempfile
import types
import unittest
from types import SimpleNamespace
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("dragonbreath_hil", ROOT / "tools" / "hil.py")
hil = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(hil)


class HilRunnerTest(unittest.TestCase):
    def test_dotted_expectations_and_operators(self):
        response = {"ok": True, "state": {"mode": "off", "count": 3}}
        hil.check_expectations(
            response,
            {
                "ok": True,
                "state.mode": "off",
                "state.count": {"gte": 2},
            },
        )

    def test_failed_expectation_is_descriptive(self):
        with self.assertRaisesRegex(hil.HilError, "expected 'off', got 'auto'"):
            hil.check_expectations(
                {"state": {"mode": "auto"}}, {"state.mode": "off"}
            )

    def test_variable_substitution(self):
        value = hil.substitute(
            {"cmd": "heartbeat", "lease_id": "$lease"}, {"lease": "abc"}
        )
        self.assertEqual(value["lease_id"], "abc")

    def test_saved_baseline_delta_expectation(self):
        expected = hil.substitute(
            {"state.count": {"delta_between": ["$before", 500, 700]}},
            {"before": 1000},
        )
        hil.check_expectations({"state": {"count": 1600}}, expected)
        with self.assertRaisesRegex(hil.HilError, "expected delta"):
            hil.check_expectations({"state": {"count": 1800}}, expected)

    def test_between_expectation(self):
        hil.check_expectations(
            {"state": {"interval_us": 8333}},
            {"state.interval_us": {"between": [6500, 10000]}},
        )
        with self.assertRaisesRegex(hil.HilError, "expected value"):
            hil.check_expectations(
                {"state": {"interval_us": 990}},
                {"state.interval_us": {"between": [6500, 10000]}},
            )

    def test_panda_smoke_checks_zero_cross_cadence(self):
        scenario = hil.load_scenario(
            hil.SCENARIO_DIR / "panda-smoke.json", "panda"
        )
        serialized = json.dumps(scenario)
        self.assertIn("zero_cross_rejected_count", serialized)
        self.assertIn("delta_between", serialized)
        self.assertIn("zero_cross_interval_us", serialized)

    def test_heat_detection(self):
        self.assertTrue(
            hil.requests_heat(
                {"cmd": "mode", "mode": "power_on", "target_c": 50}
            )
        )
        self.assertFalse(hil.requests_heat({"cmd": "off"}))
        self.assertFalse(
            hil.requests_heat({"cmd": "mode", "mode": "off", "target_c": 50})
        )

    def test_all_scenarios_parse_for_a_supported_target(self):
        for path in sorted(hil.SCENARIO_DIR.glob("*.json")):
            raw = __import__("json").loads(path.read_text(encoding="utf-8"))
            target = raw.get("targets", ["devboard", "panda"])[0]
            loaded = hil.load_scenario(path, target)
            self.assertTrue(loaded["steps"], path)
            for step in loaded["steps"]:
                if "send" in step:
                    self.assertNotIn(
                        "id",
                        step["send"],
                        f"{path}: request id is reserved for runner correlation",
                    )

    def test_each_scenario_starts_from_explicit_safe_state(self):
        for path in sorted(hil.SCENARIO_DIR.glob("*.json")):
            scenario = hil.load_scenario(path)
            first = scenario["steps"][0]
            self.assertEqual(first.get("send"), {"cmd": "off"}, path)
            self.assertEqual(first.get("expect", {}).get("state.mode"), "off", path)
            self.assertFalse(
                first.get("expect", {}).get("state.heater_output", True), path
            )

    def test_auto_scenario_injects_current_source_target_policy(self):
        scenario = hil.load_scenario(
            hil.SCENARIO_DIR / "devboard-auto.json", "devboard"
        )
        env_steps = [
            step["send"]
            for step in scenario["steps"]
            if step.get("send", {}).get("cmd") == "env"
        ]
        self.assertTrue(env_steps)
        for command in env_steps:
            self.assertEqual(command.get("src_target_c"), 55)
            self.assertEqual(command.get("chamber_src_c"), 25)
        self.assertTrue(
            any(command.get("connected") is True for command in env_steps)
        )
        self.assertTrue(
            any(command.get("connected") is False for command in env_steps)
        )

    def test_suite_only_loads_scenarios_for_target(self):
        scenarios = hil.load_suite_scenarios("devboard")
        self.assertTrue(scenarios)
        self.assertTrue(
            all("devboard" in scenario["targets"] for _, scenario in scenarios)
        )
        self.assertNotIn(
            "panda-smoke.json", {path.name for path, _ in scenarios}
        )

    def test_remote_scenarios_are_isolated_per_run(self):
        first = hil.remote_scenario_dir("/tmp/dragonbreath-hil", "run-one")
        second = hil.remote_scenario_dir("/tmp/dragonbreath-hil", "run-two")
        self.assertNotEqual(first, second)
        self.assertEqual(
            first,
            "/tmp/dragonbreath-hil/runs/run-one/tests/hil/scenarios",
        )

    def test_serial_open_releases_reset_lines(self):
        class FakeSerial:
            def __init__(self, **kwargs):
                self.port = kwargs["port"]
                self.write_timeout = kwargs["write_timeout"]
                self.rts = None
                self.dtr = None
                self.open_state = None

            def open(self):
                self.open_state = (self.rts, self.dtr)

        serial_module = types.SimpleNamespace(Serial=FakeSerial)
        with mock.patch.dict(sys.modules, {"serial": serial_module}):
            transport = hil.SerialTransport("/dev/test", 115200, io.StringIO())

        self.assertEqual(transport.serial.open_state, (True, True))
        self.assertEqual((transport.serial.rts, transport.serial.dtr), (False, False))
        self.assertEqual(transport.serial.write_timeout, 1.0)

    def test_transport_is_inferred_from_linux_port(self):
        self.assertEqual(
            hil.resolve_transport("devboard", None, "/dev/ttyACM0"), "native"
        )
        self.assertEqual(
            hil.resolve_transport("devboard", None, "/dev/ttyUSB0"), "uart"
        )
        self.assertEqual(
            hil.resolve_transport("devboard", "uart", "/dev/ttyACM0"), "uart"
        )

    def test_panda_rejects_native_usb_transport(self):
        with self.assertRaisesRegex(hil.HilError, "Panda HIL uses UART"):
            hil.resolve_transport("panda", "native", "/dev/ttyACM0")

    def test_uart_devboard_uses_dedicated_profile(self):
        args = SimpleNamespace(
            target="devboard", transport="uart", build_dir=None
        )
        overlay, build_dir = hil.profile_paths(args)
        self.assertEqual(overlay.name, "sdkconfig.hil-devboard-uart")
        self.assertEqual(build_dir.name, "build-hil-devboard-uart")

    def test_relative_build_dir_is_rooted_in_repository(self):
        args = SimpleNamespace(
            target="devboard", transport="native", build_dir="build-custom"
        )
        _, build_dir = hil.profile_paths(args)
        self.assertEqual(build_dir, ROOT / "build-custom")

    def test_local_builds_use_guarded_idf_wrapper(self):
        args = SimpleNamespace(idf="idf.py")
        build_dir = ROOT / "build-test"
        command = hil.guarded_idf_command(args, build_dir)
        self.assertEqual(command[0], str(ROOT / "tools" / "idf-build.sh"))
        self.assertEqual(command[1:4], [str(ROOT), "esp32c3", str(build_dir)])

    def test_remote_flash_command_uses_generated_manifest(self):
        with tempfile.TemporaryDirectory() as temp:
            build_dir = pathlib.Path(temp)
            (build_dir / "bootloader").mkdir()
            (build_dir / "bootloader" / "bootloader.bin").write_bytes(b"boot")
            (build_dir / "dragonbreath.bin").write_bytes(b"app")
            manifest = {
                "write_flash_args": ["--flash_mode", "dio"],
                "flash_files": {
                    "0x20000": "dragonbreath.bin",
                    "0x0": "bootloader/bootloader.bin",
                },
                "extra_esptool_args": {
                    "chip": "esp32c3",
                    "before": "default_reset",
                    "after": "hard_reset",
                    "stub": False,
                },
            }
            (build_dir / "flasher_args.json").write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            args = SimpleNamespace(
                remote_esptool="/venv/bin/esptool.py",
                port="/dev/ttyACM0",
                flash_baud=460800,
            )

            command, sources = hil.remote_flash_command(
                args, build_dir, "/tmp/hil/firmware"
            )

        self.assertIn("--no-stub", command)
        self.assertLess(command.index("0x0"), command.index("0x20000"))
        self.assertIn("/tmp/hil/firmware/dragonbreath.bin", command)
        self.assertEqual(
            {path.name for path in sources},
            {"bootloader.bin", "dragonbreath.bin"},
        )


if __name__ == "__main__":
    unittest.main()
