"""将主控制器的标定模式反馈映射为网页只读数据；不发命令、不更改标定。"""
import math

from .startup_status import imu_context_current

# 标定模式 IPC 每 100 ms 轮询；网页允许五拍传输延迟，不改变控制器的 50 ms 门限。
DISPLAY_MAX_AGE_MS = 500
NATIVE_MAX_AGE_MS = 50


def _finite_number(value):
    """仅接受有限数值，避免布尔值、NaN 或缺失数据变成有效反馈。"""
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def commissioning_view(rd, now=None):
    """读取本轮主程序的反馈，合并源端和接收端年龄；旧数据返回未知字段。

    非标定模式返回 None，保留现有运动模式路径。扭矩状态来自控制器已确认的
    状态机，不是本帧逐颗寄存器读数；只有完整、新鲜的反馈才能显示全部卸力。
    """
    if rd is None:
        return None
    bus = rd.bus.value
    if not isinstance(bus, dict) or bus.get("mode") != "commissioning":
        return None
    sample = bus.get("last_sample")
    native = bus.get("native")
    native = native if isinstance(native, dict) else {}
    if not isinstance(sample, dict) and isinstance(native.get("servo_observations"), dict):
        # 新版启动诊断可独立报告舵机和 ID 200；尚无采样时不能用空表覆盖它。
        return None
    sample = sample if isinstance(sample, dict) else {}
    imu = sample.get("imu")
    imu = imu if isinstance(imu, dict) else {}
    updated_us, sampled_us = bus.get("updated_at_us"), sample.get("at_us")
    native_age_ms = None
    if (_finite_number(updated_us) and _finite_number(sampled_us)
            and updated_us >= sampled_us >= 0):
        native_age_ms = (updated_us - sampled_us) / 1000
    receipt_s = rd.bus.age(now)
    context_current = imu_context_current(rd, now)
    age_ms = None
    if context_current and native_age_ms is not None and _finite_number(receipt_s):
        age_ms = native_age_ms + receipt_s * 1000
    fresh = (age_ms is not None and 0 <= age_ms < DISPLAY_MAX_AGE_MS
             and native_age_ms <= NATIVE_MAX_AGE_MS
             and bus.get("last_cycle_error") is None
             and bus.get("phase") in ("ready", "holding", "moving"))

    rows = {}
    for row in sample.get("servos") if isinstance(sample.get("servos"), list) else []:
        if not isinstance(row, dict):
            continue
        servo_id, raw = row.get("id"), row.get("position_raw")
        if not isinstance(servo_id, int) or isinstance(servo_id, bool):
            continue
        if servo_id in rows:
            # 重复 ID 不能任选一帧，避免把格式异常的数据当成完整回读。
            rows[servo_id] = None
        elif fresh and isinstance(raw, int) and not isinstance(raw, bool) and 0 <= raw <= 4095:
            rows[servo_id] = {
                "id": servo_id, "name": row.get("joint"), "position_raw": raw,
                **{key: row.get(key) if _finite_number(row.get(key)) else None
                   for key in ("voltage_v", "temperature_c", "load_raw", "speed_raw", "current_raw")},
                "moving": row.get("moving") if isinstance(row.get("moving"), bool) else None,
            }
        else:
            rows[servo_id] = None
    rows = {servo_id: row for servo_id, row in rows.items() if row is not None}

    imu_age = imu.get("age_ms")
    gravity = imu.get("gravity")
    imu_fresh = (fresh and imu.get("valid") is True and imu.get("ready") is True
                 and _finite_number(imu_age) and 0 <= imu_age <= NATIVE_MAX_AGE_MS
                 and age_ms + imu_age < DISPLAY_MAX_AGE_MS
                 and isinstance(gravity, list) and len(gravity) == 3
                 and all(_finite_number(value) for value in gravity))
    mount_verified = imu.get("mount_verified", bus.get("imu_mount_verified"))
    # 缺少核验标志也不显示“正常”；已有 health.ready 不代表安装方向正确。
    imu_status = "missing"
    if imu_fresh:
        imu_status = "ok" if mount_verified is True else "unverified"
    source = imu.get("source") if context_current else None
    torque = bus.get("torque_state") if fresh else None
    return {
        "fresh": fresh, "age_ms": age_ms, "rows": rows,
        "torque": torque if torque in ("off", "all_enabled", "single_joint_enabled") else None,
        "torque_source": "controller", "imu_status": imu_status,
        "imu_source": source if isinstance(source, str) else None,
        "imu_fresh": imu_fresh, "imu_age_ms": age_ms + imu_age if imu_fresh else None,
        "gravity": gravity if imu_fresh else None,
        "state_t_ns": int(sampled_us * 1000) if imu_fresh else None,
        "hz": bus.get("achieved_hz") if fresh and _finite_number(bus.get("achieved_hz")) else None,
    }
