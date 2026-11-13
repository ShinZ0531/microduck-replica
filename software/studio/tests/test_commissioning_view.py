"""核对实机标定反馈的只读显示，以及断线、缓存、缺帧时的未知状态。"""
import copy
import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from studio.commissioning_view import commissioning_view
from studio.estop import EStop
from studio.imu import ImuBackend
from studio.robotd import Sample
from studio.servo import IDS, view_from_robotd
from studio.state import summarize


class CommissioningViewTests(unittest.TestCase):
    """以已保存的板端真实快照为基线，不连接网络或舵机串口。"""

    def setUp(self):
        """创建接收年龄为 10 ms 的主控缓存，保留未核验安装方向。"""
        fixture = Path(__file__).resolve().parent / "fixtures" / "head-bmi088-commissioning.json"
        self.bus = json.loads(fixture.read_text(encoding="utf-8-sig"))
        self.clock = patch("time.monotonic", return_value=10.0)
        self.clock.start()
        self.addCleanup(self.clock.stop)
        samples = {key: Sample() for key in ("bus", "hello", "health", "policies", "state", "pad", "sysinfo", "calib")}
        self.rd = SimpleNamespace(**samples, present=True, estop=EStop(), startup=None)
        self.rd.bus.set(self.bus)
        self.rd.bus.at = 9.99
        self.rd.hello.at = 9.0
        self.rd.health.set({"imu": {"ready": True}})

    def assert_unknown(self):
        """确认旧回读不能显示在线、卸力确认或有效躯干姿态。"""
        view = view_from_robotd(self.rd, None)
        state = summarize(self.rd, "test", now=10.0)
        self.assertIsNone(view["online"])
        self.assertFalse(view["all_off"])
        self.assertTrue(all(row["raw"] is None and row["torque"] is None for row in view["rows"]))
        self.assertEqual(state["imu"]["status"], "missing")
        self.assertIsNone(state["gravity"])

    def test_real_complete_snapshot_is_read_only_and_unverified(self):
        """完整实测帧显示 15 颗、控制器卸力和头部来源，不把 ready 当已校准。"""
        view = view_from_robotd(self.rd, None)
        state = summarize(self.rd, "test", now=10.0)
        self.assertEqual(view["state"], "closed")
        self.assertEqual(view["source"], "robotd")
        self.assertEqual(view["online"], 15)
        self.assertTrue(view["all_off"])
        self.assertEqual(view["holding"], 0)
        self.assertEqual({row["id"] for row in view["rows"]}, set(IDS))
        self.assertTrue(all(row["fresh"] and row["torque_source"] == "controller" for row in view["rows"]))
        self.assertEqual(state["servos"]["online"], 15)
        self.assertEqual(state["imu"]["source"], "head_bmi088")
        self.assertEqual(state["imu"]["status"], "unverified")
        self.assertEqual(state["gravity"], self.bus["last_sample"]["imu"]["gravity"])
        self.assertFalse(self.bus["imu_mount_verified"])
        self.assertFalse(self.bus["policy_motion_enabled"])

    def test_mechanical_angle_uses_feedback_and_direction(self):
        """显示角度沿用舵机页零位和方向约定，不使用指令目标。"""
        self.bus["last_sample"]["servos"][0]["position_raw"] = 2560
        view = view_from_robotd(self.rd, None)
        self.assertEqual(view["rows"][0]["deg"], -45.0)
        self.assertEqual(view_from_robotd(self.rd, {20: 1})["rows"][0]["deg"], 45.0)

    def test_missing_or_duplicate_servo_cannot_confirm_all_off(self):
        """少一颗或同 ID 重复时，该关节未知，不能显示全部卸力。"""
        original = copy.deepcopy(self.bus["last_sample"]["servos"])
        for rows in (original[:-1], original + [original[0]]):
            with self.subTest(count=len(rows)):
                self.bus["last_sample"]["servos"] = rows
                view = view_from_robotd(self.rd, None)
                self.assertEqual(view["online"], 14)
                self.assertFalse(view["all_off"])

    def test_source_cache_age_is_not_reset_by_fresh_rpc(self):
        """刚收到的 IPC 若包含陈旧源帧，仍必须拒绝。"""
        self.bus["updated_at_us"] = self.bus["last_sample"]["at_us"] + 50001
        self.assert_unknown()

    def test_stale_receipt_is_unknown(self):
        """主控不更新时，超五拍的网页缓存失效。"""
        self.rd.bus.at = 9.49
        self.assert_unknown()

    def test_reconnect_rejects_previous_daemon_snapshot(self):
        """新 hello 之后不能显示上个进程的快照。"""
        self.rd.hello.at = 10.0
        self.assert_unknown()

    def test_ipc_failure_and_absent_daemon_are_unknown(self):
        """查询失败或连接断开不能沿用旧的卸力或 IMU 正常状态。"""
        self.rd.bus.error = "socket closed"
        self.assert_unknown()
        self.rd.bus.error = None
        self.rd.present = False
        self.assert_unknown()

    def test_failed_cycle_and_unknown_timestamps_are_unknown(self):
        """失败循环、负时差及缺失源时钟都不能把 last_sample 当实时。"""
        original = copy.deepcopy(self.bus)
        for change in ({"last_cycle_error": "no feedback"}, {"updated_at_us": None},
                       {"updated_at_us": 0}, {"phase": "stop_unconfirmed"}):
            with self.subTest(change=change):
                self.bus.clear()
                self.bus.update(copy.deepcopy(original))
                self.bus.update(change)
                self.assert_unknown()

    def test_bad_imu_does_not_invent_orientation(self):
        """无效或过期 IMU 清空姿态，关节的有效回读仍保留。"""
        original = copy.deepcopy(self.bus["last_sample"]["imu"])
        for change in ({"valid": False}, {"ready": False}, {"age_ms": 51},
                       {"gravity": [float("nan"), 0, -1]}, {"gravity": None}):
            with self.subTest(change=change):
                self.bus["last_sample"]["imu"] = {**original, **change}
                state = summarize(self.rd, "test", now=10.0)
                self.assertEqual(state["imu"]["status"], "missing")
                self.assertIsNone(state["gravity"])
                self.assertEqual(state["servos"]["online"], 15)

    def test_unknown_mount_and_partial_enable_are_not_verified(self):
        """缺少方向标志和单关节使能均不能推导成全机已核验/已卸力。"""
        del self.bus["imu_mount_verified"]
        self.bus["last_sample"]["imu"].pop("mount_verified", None)
        self.bus["torque_state"] = "single_joint_enabled"
        self.assertEqual(commissioning_view(self.rd)["imu_status"], "unverified")
        view = view_from_robotd(self.rd, None)
        self.assertFalse(view["all_off"])
        self.assertTrue(all(row["torque"] is None for row in view["rows"]))

    def test_legacy_motion_path_is_preserved(self):
        """普通运动模式仍使用原 native.joints 路径。"""
        self.rd.bus.value = {"mode": "motion", "native": {"joints": [
            {"id": 20, "position_raw": 2048, "feedback_fresh": True, "volts": 7.6}]}}
        self.assertIsNone(commissioning_view(self.rd))
        self.assertEqual(view_from_robotd(self.rd, None)["online"], 1)
        self.assertEqual(view_from_robotd(self.rd.bus.value, None)["rows"][0]["deg"], 0.0)

    def test_imu_page_uses_same_freshness_as_overview(self):
        """独立 IMU 消息不能绕过总览的过期保护继续显示旧姿态。"""
        backend = ImuBackend(self.rd, None)
        with patch("studio.imu.helper.imu_redo_state", return_value={}), \
                patch("studio.imu.helper.flag_present", return_value=False), \
                patch.object(backend, "_stand_calib_at", return_value=None):
            current = backend.view()
            self.assertEqual(current["source"], "head_bmi088")
            self.assertTrue(current["valid"])
            self.assertTrue(current["ready"])
            self.assertEqual(current["reader"]["state"], "streaming")
            self.bus["last_cycle_error"] = "head joint limit"
            stale = backend.view()
            self.assertFalse(stale["valid"])
            self.assertFalse(stale["ready"])
            self.assertIsNone(stale["gravity"])
            self.assertIsNone(stale["age_ms"])
            self.assertEqual(stale["reader"]["state"], "missing")


if __name__ == "__main__":
    unittest.main()
