"""Exercise malformed/stale data and one shared Unix subscription without robot commands."""
import asyncio
import contextlib
import json
from pathlib import Path
import tempfile
import time
import unittest

from studio.head_imu import HeadImuReader, validated_frame


FRAME = {"seq": 1, "t_ns": 1, "accel": [0.0, 0.0, 9.81],
         "gyro": [0.0, 0.0, 0.0], "quat": [1.0, 0.0, 0.0, 0.0], "temp_c": 25.0}


class FrameTests(unittest.TestCase):
    """Reject broken sensor reports and distinguish a real zero from a missing sample."""

    def test_invalid_fields(self):
        """NaN, booleans, missing vectors and zero quaternions cannot become a live pose."""
        for update in ({"accel": [float("nan"), 0, 1]}, {"seq": True},
                       {"gyro": None}, {"quat": [0, 0, 0, 0]}, {"temp_c": float("inf")}):
            with self.subTest(update=update), self.assertRaises(ValueError):
                validated_frame({**FRAME, **update})
        self.assertEqual(validated_frame(FRAME)["gyro"], [0, 0, 0])

    def test_stale_and_disconnected(self):
        """A retained last sample cannot masquerade as current data after expiry or disconnect."""
        reader = HeadImuReader()
        reader.frame = {**FRAME, "t_ns": time.monotonic_ns() - 1000000000}
        reader.connected = True
        reader.received_at = time.monotonic() - 1
        self.assertIsNone(reader.snapshot()["frame"])
        reader.received_at = time.monotonic()
        self.assertFalse(reader.snapshot()["fresh"])
        reader.frame = {**FRAME, "t_ns": time.monotonic_ns()}
        self.assertTrue(reader.snapshot()["fresh"])
        reader.connected = False
        self.assertFalse(reader.snapshot()["fresh"])

    def test_future_timestamp_and_source_rate(self):
        """未来源时间不是有效样本；频率使用采样时刻，不使用集中到达时刻。"""
        reader = HeadImuReader()
        reader.connected = True
        now = time.monotonic()
        reader.frame = {**FRAME, "t_ns": int((now + 1) * 1e9)}
        reader.received_at = now
        self.assertFalse(reader.snapshot()["fresh"])
        reader.frame = {**FRAME, "t_ns": int(now * 1e9)}
        reader.frame_times.extend(now - (9 - index) * 0.02 for index in range(10))
        self.assertAlmostEqual(reader.snapshot()["measured_hz"], 50.0, places=1)


class StreamTests(unittest.IsolatedAsyncioTestCase):
    """Use a temporary Unix producer to check fan-out, duplicate expiry and cleanup."""

    async def test_shared_subscription_and_duplicate_expiry(self):
        """Concurrent viewers share one socket; repeated sequence IDs never renew freshness."""
        connections = []
        ended = asyncio.Event()
        frame = {**FRAME, "t_ns": time.monotonic_ns()}

        async def producer(socket_reader, socket_writer):
            """Send a single sample repeatedly so freshness must expire despite traffic."""
            connections.append(1)
            request = json.loads(await socket_reader.readline())
            self.assertEqual(request["method"], "head_imu.stream")
            socket_writer.write((json.dumps({"jsonrpc": "2.0", "id": 1,
                "result": {"accepted": True, "sensor": "BMI088", "hz": 50}}) + "\n").encode())
            try:
                while not ended.is_set():
                    socket_writer.write((json.dumps({"jsonrpc": "2.0", "method": "head_imu.frame", "params": frame}) + "\n").encode())
                    await socket_writer.drain()
                    await asyncio.sleep(0.02)
            except (ConnectionError, BrokenPipeError):
                pass  # The test deliberately closes its subscriber.
            finally:
                socket_writer.close()
                with contextlib.suppress(ConnectionError):
                    await socket_writer.wait_closed()

        with tempfile.TemporaryDirectory() as directory:
            socket_path = str(Path(directory) / "imu.sock")
            server = await asyncio.start_unix_server(producer, path=socket_path)
            reader = HeadImuReader("unix:" + socket_path)
            try:
                await asyncio.gather(reader.view(), reader.view())
                await asyncio.sleep(0.1)
                self.assertTrue(reader.snapshot()["fresh"])
                await asyncio.sleep(0.6)
                self.assertFalse(reader.snapshot()["fresh"])
                self.assertIsNone(reader.snapshot()["frame"])
                self.assertEqual(len(connections), 1)
            finally:
                ended.set()
                await reader.close()
                self.assertIsNone(reader.task)
                server.close()
                await server.wait_closed()
                await asyncio.sleep(0.05)


if __name__ == "__main__":
    unittest.main()
