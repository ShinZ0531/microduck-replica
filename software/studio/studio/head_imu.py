"""只读订阅 HAT BMI088；不修改 robotd 或标定文件。"""
import asyncio
from collections import deque
import contextlib
import json
import math
import time


FRESH_SECONDS = 0.5
IDLE_SECONDS = 15.0


def finite_vector(value, length):
    """校验定长传感器向量；拒绝布尔值、NaN 和无穷值。"""
    return (isinstance(value, list) and len(value) == length
            and all(type(item) in (float, int) and math.isfinite(item) for item in value))


def validated_frame(data):
    """校验带单位的字段，并归一化服务提供的 w,x,y,z 四元数。"""
    if not isinstance(data, dict):
        raise ValueError("IMU 帧格式不正确")
    for field, length in (("accel", 3), ("gyro", 3), ("quat", 4)):
        if not finite_vector(data.get(field), length):
            raise ValueError(f"IMU {field} 数据无效")
    for field in ("seq", "t_ns"):
        if type(data.get(field)) is not int or not 0 <= data[field] < 2**64:
            raise ValueError(f"IMU {field} 无效")
    norm = math.hypot(*data["quat"])
    if not 0.9 <= norm <= 1.1:
        raise ValueError("IMU 姿态四元数无效")
    temperature = data.get("temp_c")
    if temperature is not None and not finite_vector([temperature], 1):
        raise ValueError("IMU 温度无效")
    return {"seq": data["seq"], "t_ns": data["t_ns"],
            "accel": data["accel"], "gyro": data["gyro"],
            "quat": [item / norm for item in data["quat"]], "temp_c": temperature}


class HeadImuReader:
    """多个已登录查看者共用有界、按需启动的只读流。"""

    def __init__(self, address="unix:/run/tofd/tof.sock"):
        """保存本地服务地址；使用单调时间，避免依赖板端墙钟。"""
        self.address = address
        self.task = None
        self.frame = None
        self.received_at = None
        self.requested_at = 0.0
        self.connected = False
        self.reason = "正在连接 HAT IMU"
        self.expected_hz = None
        self.frame_times = deque(maxlen=100)
        self.session = 0

    async def view(self):
        """返回最新有效帧；过期或缺失读数为 null，不伪造零姿态。"""
        self.requested_at = time.monotonic()
        if self.task is None or self.task.done():
            self.task = asyncio.create_task(self._collect())
            await asyncio.sleep(0)
        return self.snapshot()

    def snapshot(self):
        """按源采样时刻计算年龄和更新率，排队到达不能让旧包变新。"""
        # tofd 的 t_ns 明确定义为板端 CLOCK_MONOTONIC；排队到达不能刷新旧样本的年龄。
        sample_at = None if self.frame is None else self.frame["t_ns"] / 1e9
        age = None if sample_at is None else time.monotonic() - sample_at
        fresh = self.connected and age is not None and 0 <= age < FRESH_SECONDS
        measured_hz = None
        if len(self.frame_times) >= 10:
            duration = self.frame_times[-1] - self.frame_times[0]
            if duration > 0:
                measured_hz = round((len(self.frame_times) - 1) / duration, 1)
        return {"state": "live" if fresh else "waiting", "sensor": "BMI088",
                "fresh": fresh, "age_ms": None if age is None else round(age * 1000),
                "expected_hz": self.expected_hz, "measured_hz": measured_hz if fresh else None,
                "session": self.session, "frame": self.frame if fresh else None,
                "reason": None if fresh else self.reason or "IMU 数据未更新",
                "frame_name": "sensor", "mount_verified": False}

    async def _read_message(self, reader):
        """限长、限时读取一行 JSON-RPC，使空闲或断线订阅能结束。"""
        line = await asyncio.wait_for(reader.readline(), timeout=1.0)
        if not line:
            raise ConnectionError("HAT IMU 服务连接已断开")
        message = json.loads(line)
        if not isinstance(message, dict) or message.get("jsonrpc") != "2.0":
            raise ValueError("IMU 服务协议不正确")
        return message

    async def _collect(self):
        """页面需要时重连只读流；取消或超时后释放套接字。"""
        while time.monotonic() - self.requested_at < IDLE_SECONDS:
            writer = None
            self.connected = False
            self.frame = None
            self.received_at = None
            self.frame_times.clear()
            try:
                if not isinstance(self.address, str) or not self.address.startswith("unix:"):
                    raise ValueError("未配置 HAT IMU 本地服务")
                reader, writer = await asyncio.wait_for(
                    asyncio.open_unix_connection(self.address[5:], limit=16384), timeout=1.0)
                writer.write(b'{"jsonrpc":"2.0","id":1,"method":"head_imu.stream","params":{}}\n')
                await writer.drain()
                ack = await self._read_message(reader)
                metadata = ack.get("result")
                if (ack.get("id") != 1 or not isinstance(metadata, dict)
                        or metadata.get("accepted") is not True or metadata.get("sensor") != "BMI088"):
                    raise ValueError("HAT BMI088 订阅未被服务接受")
                hz = metadata.get("hz")
                self.expected_hz = hz if type(hz) in (int, float) and 0 < hz <= 1000 else None
                self.connected = True
                self.session += 1
                self.reason = "正在等待 BMI088 数据"
                previous_seq = None
                previous_time = None
                while time.monotonic() - self.requested_at < IDLE_SECONDS:
                    message = await self._read_message(reader)
                    if message.get("method") != "head_imu.frame":
                        continue
                    frame = validated_frame(message.get("params"))
                    # 重复或乱序帧不能刷新数据年龄。
                    if previous_seq is not None and (frame["seq"] <= previous_seq or frame["t_ns"] <= previous_time):
                        self.reason = "IMU 帧序号或时间未推进"
                        continue
                    previous_seq, previous_time = frame["seq"], frame["t_ns"]
                    self.frame = frame
                    self.received_at = time.monotonic()
                    self.frame_times.append(frame["t_ns"] / 1e9)
                    self.reason = None
            except (OSError, ValueError, asyncio.TimeoutError, ConnectionError) as error:
                self.reason = "IMU 数据超时，正在重连" if isinstance(error, asyncio.TimeoutError) else str(error)
            finally:
                self.connected = False
                if writer is not None:
                    writer.close()
                    with contextlib.suppress(OSError, asyncio.TimeoutError):
                        await asyncio.wait_for(writer.wait_closed(), timeout=1.0)
            await asyncio.sleep(0.8)

    async def close(self):
        """Studio 关闭时取消订阅，等待套接字清理完成。"""
        if self.task is not None:
            self.task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self.task
            self.task = None
