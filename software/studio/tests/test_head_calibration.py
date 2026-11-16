"""头部专用核验的来源、证据、保存及模式交接回归；只使用模拟系统和临时文件。"""
import asyncio
import copy
import json
import tempfile
import unittest
from unittest import mock

from studio import commit as C
from studio.imu import ImuBackend, Refused
from tests.test_commit import Rd, Log
from tests.test_helper_calib import Base as HelperBase, H, current_calibration, joints_payload


def head_fixture():
    """生成主控已核验形状的报告，并使用非出厂零位验证写入不会覆盖用户数据。"""
    calibration = current_calibration()
    calibration["joints"][6]["zero_raw"] = 2000
    quat = [0.5, 0.5, 0.5, 0.5]
    calibration["imu_mounts"]["head_bmi088"] = {"quat": quat, "verified": True}
    proof = {
        "contract": "head_mount_standing_v2", "source": "head_bmi088", "mount": quat,
        "binding": {"model_sha256": "078693dff3bcd5e69eb6cc483974178e1511a11f762b1a4dd00370bc5d58b8a8",
                    "joints": [{key: row.get(key) for key in ("id", "name", "zero_raw", "direction")}
                               for row in calibration["joints"] if row["id"] in (30, 31, 32, 33)]},
        "reports": [{"step": step, "sample_count": 100, "sample_span_s": 1.98,
                     "max_body_error_deg": 0.5, "body_gyro_rms_rad_s": 0.02, "body_gyro_peak_rad_s": 0.05,
                     "gyro_relative_error_ratio": 0.1, "head_span_deg": 12.0 if 6 <= step <= 9 else 0.0} for step in range(10)],
    }
    return calibration, proof


class HeadProofTest(unittest.TestCase):
    """覆盖可能把未核验状态错误升级为通过的纯数据路径。"""

    def test_proof_binds_head_geometry_and_measurements(self):
        """报告缺步、关节几何改变及超差残差均被拒绝。"""
        calibration, proof = head_fixture()
        C.require_head_proof(calibration, proof, proof["mount"])
        for change in ("missing", "zero", "gyro", "error", "span", "ratio", "old_contract"):
            changed, record = copy.deepcopy(calibration), copy.deepcopy(proof)
            if change == "missing":
                record["reports"].pop()
            elif change == "old_contract":
                record["contract"] = "head_mount_fixed_torso_v1"
            elif change == "zero":
                changed["joints"][6]["zero_raw"] += 1
            else:
                key, value = {"gyro": ("body_gyro_rms_rad_s", 0.3), "error": ("max_body_error_deg", 8.0),
                              "span": ("head_span_deg", 1.0), "ratio": ("gyro_relative_error_ratio", 1.0)}[change]
                record["reports"][6][key] = value
            with self.subTest(change=change), self.assertRaises(C.CalibError):
                C.require_head_proof(changed, record, proof["mount"])

    def test_build_preserves_zero_direction_and_existing_limits(self):
        """两份纯函数均保留实测零位和已有限位，同时只填充原配置中缺失的通用限位。"""
        calibration, proof = head_fixture()
        calibration["joints"][0].update(min_rad=-0.3, max_rad=0.4)
        outputs = []
        for module in (C, H):
            built = module.build_calibration(calibration, joints_payload(),
                                             {"source": "head_bmi088", "quat": proof["mount"]})
            module.validate_calibration(built, "head_bmi088")
            self.assertEqual(built["joints"][6]["zero_raw"], 2000)
            self.assertEqual(built["joints"][0]["min_rad"], -0.3)
            self.assertEqual(built["joints"][0]["max_rad"], 0.4)
            C.require_head_proof(built, proof, proof["mount"])
            outputs.append(module.render_calibration(built))
        self.assertEqual(*outputs)

    def test_builder_cannot_certify_an_arbitrary_mount(self):
        """与主控已存安装不同的四元数、或改过的方向，不能由第三步直接变成 verified。"""
        calibration, proof = head_fixture()
        for module in (C, H):
            with self.assertRaises(module.CalibError):
                module.build_calibration(calibration, joints_payload(),
                                         {"source": "head_bmi088", "quat": [1.0, 0.0, 0.0, 0.0]})
            with self.assertRaises(module.CalibError):
                module.build_calibration(calibration, joints_payload({"head_pitch": 1}),
                                         {"source": "head_bmi088", "quat": proof["mount"]})

    def test_legacy_pending_is_not_head_verification(self):
        """固定躯干求解的暂存结果不能用于头部核验。"""
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(C.Refused):
                C.save_pending(directory, "head_bmi088", [1.0, 0.0, 0.0, 0.0])


class HeadBackendTest(unittest.IsolatedAsyncioTestCase):
    """专用网页接口只转发步骤，不让客户端上传通过依据。"""

    async def test_native_steps_only_and_legacy_path_refused(self):
        """页面原始样本、四元数及固定躯干操作均不能混入头部流程。"""
        calibration, proof = head_fixture()
        with tempfile.TemporaryDirectory() as directory:
            rd = Rd({"calibration": calibration, "imu_source": "head_bmi088"}, source="head_bmi088")
            backend = ImuBackend(rd, Log(), directory)
            backend._bus_once = mock.AsyncMock(return_value=rd.bus.value)
            backend._rpc_once = mock.AsyncMock(return_value={"active": True, "next_step": 1})
            result = await backend.handle({"op": "head-test", "step": 0, "confirmed": True})
            self.assertEqual(result["next_step"], 1)
            self.assertEqual(backend._rpc_once.call_args.args[1], {
                "action": "head-imu-calibration", "operation": "test", "step": 0, "confirmed": True})
            for args in ({"op": "head-test", "step": 0, "confirmed": True, "samples": []},
                         {"op": "head-save", "confirmed": True, "mount": proof["mount"]},
                         {"op": "head-test", "step": True, "confirmed": True},
                         {"op": "verify", "confirmed": True}):
                with self.subTest(args=args), self.assertRaises(Refused):
                    await backend.handle(args)

    async def test_head_source_keeps_the_original_standing_enable_entry(self):
        """头部来源复用原站姿使能入口，不再被专用流程的来源检查误拦。"""
        calibration, _ = head_fixture()
        with tempfile.TemporaryDirectory() as directory:
            rd = Rd({"calibration": calibration, "imu_source": "head_bmi088"}, source="head_bmi088")
            backend = ImuBackend(rd, Log(), directory)
            backend._hold = mock.AsyncMock(return_value={"holding": True})
            self.assertEqual(await backend.handle({"op": "hold"}), {"holding": True})
            backend._hold.assert_awaited_once()


class HeadCommitTest(HelperBase):
    """在假根目录内验收头部第三步的切换和失败回退。"""

    def setUp(self):
        """保留原助手模拟设施，安装已知诊断覆盖和匹配的十步报告。"""
        super().setUp()
        self.calibration, self.proof = head_fixture()
        self.old = H.render_calibration(self.calibration).encode()
        self.path.write_bytes(self.old)
        self.commissioning(True)
        self.head_override = self.w.path(H.HEAD_DIAGNOSTIC_DROPIN)
        self.head_override.parent.mkdir(parents=True, exist_ok=True)
        self.head_before = (H.HEAD_OVERRIDE_PREFIX + H.HEAD_START + " --commissioning\n").encode()
        self.head_override.write_bytes(self.head_before)
        self.proof_file = self.w.path(H.HEAD_PROOF)
        self.proof_file.write_text(json.dumps(self.proof))

    def tearDown(self):
        """只清理本测试临时根目录，不访问真实系统。"""
        super().tearDown()

    def status(self, _op):
        """从模拟当前标定文件读回实际写入值，不能直接回显预期 payload。"""
        current = H.tomllib.loads(self.path.read_text())
        return {"calibration": current, "imu_source": "head_bmi088",
                "imu_mount": current["imu_mounts"]["head_bmi088"], "head_imu_verification": self.proof}, None

    def bus(self, _op):
        """重启前断力标定、重启后运动控制；两阶段均为模拟回读。"""
        return {"mode": "motion" if self.w.restarted else "commissioning",
                "phase": "control" if self.w.restarted else "ready", "torque_state": "off"}, None

    def test_known_override_switches_after_real_proof_and_preserves_zero(self):
        """写入成功后保留自定义二进制，只去掉已知 commissioning 参数，且报告保持不变。"""
        payload = self.payload(imu={"source": "head_bmi088", "quat": self.proof["mount"]})
        with mock.patch.object(H, "calibration_status", side_effect=self.status), \
                mock.patch.object(H, "robotd_bus_status", side_effect=self.bus):
            code, _ = self.call(payload)
        self.assertEqual(code, H.EXIT_OK, self.last())
        self.assertNotIn(b"--commissioning", self.head_override.read_bytes())
        self.assertIn(H.HEAD_START.encode(), self.head_override.read_bytes())
        self.assertEqual(H.tomllib.loads(self.path.read_text())["joints"][6]["zero_raw"], 2000)
        self.assertEqual(json.loads(self.proof_file.read_text()), self.proof)

    def test_unknown_override_is_rejected_without_file_write(self):
        """人工改过的服务覆盖不得被自动删参数或改写标定。"""
        self.head_override.write_bytes(self.head_before + b"Environment=OTHER=1\n")
        payload = self.payload(imu={"source": "head_bmi088", "quat": self.proof["mount"]})
        with mock.patch.object(H, "calibration_status", side_effect=self.status), \
                mock.patch.object(H, "robotd_bus_status", side_effect=self.bus):
            code, _ = self.call(payload)
        self.assertNotEqual(code, H.EXIT_OK)
        self.assertEqual(self.path.read_bytes(), self.old)
        self.assertFalse(self.w.restarted)

    def test_failed_readback_restores_head_override_and_calibration(self):
        """切换后主控不启动时，恢复原始标定和额外头部服务覆盖。"""
        self.w.hello_ok = False
        payload = self.payload(imu={"source": "head_bmi088", "quat": self.proof["mount"]})
        with mock.patch.object(H, "calibration_status", side_effect=self.status), \
                mock.patch.object(H, "robotd_bus_status", side_effect=self.bus):
            code, _ = self.call(payload)
        self.assertNotEqual(code, H.EXIT_OK)
        self.assertEqual(self.path.read_bytes(), self.old)
        self.assertEqual(self.head_override.read_bytes(), self.head_before)
