#pragma once

#include "head_calibration.h"

#include <array>
#include <deque>

namespace microduck
{
    /**
     * 核验中位、两侧及回中位四个停稳端点，并逐段比较陀螺积分与实测关节旋转。
     * 输入按 IMU 源时间递增，安装为 sensor→head-site；失败抛异常，由 C ABI 边界转换。
     * 角速度峰值/RMS 仅报告，不以运动过程卡顿判安装失败；不会控制电机或保存配置。
     */
    void verifyHeadEndpoints(const std::deque<MicroduckHeadInput>& frames,
                             const std::array<double, 4>& mount,
                             MicroduckHeadCalibrationStatus& status);
}
