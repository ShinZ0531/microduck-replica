#include "head_endpoint_verification.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{
    using Rotation = std::array<double, 4>;
    using Vector = std::array<double, 3>;
    constexpr double kDegrees = 57.29577951308232;
    // ±5° 台架核验门限：0.6 s 端点不要求执行匀速；3° 姿态误差仍能拒绝反向的约 10° 差。
    constexpr unsigned long long kSettledNs = 600000000;
    constexpr unsigned long long kMaximumGapNs = 60000000;
    constexpr double kSettledJointSpreadDeg = 0.5;
    constexpr double kSettledPoseSpreadDeg = 1.0;
    constexpr double kSettledGyroRmsRadS = 0.08;
    constexpr double kMaximumEndpointErrorDeg = 3.0;
    constexpr double kMinimumLegDeg = 3.5;
    constexpr double kMaximumIntegralRatio = 0.5;

    /** 停稳窗口的平均姿态、关节位置和偏置；middle 是积分边界的真实帧下标。 */
    struct Endpoint
    {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t middle = 0;
        Rotation sensor{1.0, 0.0, 0.0, 0.0};
        std::array<double, 4> angles{};
        Vector bias{};
    };

    /** Hamilton 组合；陀螺的传感器局部旋转右乘到 sensor→起始姿态。 */
    Rotation multiply(const Rotation& a, const Rotation& b)
    {
        return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
                a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
                a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
                a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
    }

    /** 归一化旋转；均值退化及非法数值不得生成有效姿态。 */
    Rotation normalized(Rotation value)
    {
        double squared = 0.0;
        for (double item : value)
        {
            squared += item * item;
        }
        if (!std::isfinite(squared) || squared < 1e-12)
        {
            throw std::runtime_error("端点姿态无效，请重新采集当前关节");
        }
        for (double& item : value)
        {
            item /= std::sqrt(squared);
        }
        return value;
    }

    /** 单位旋转取逆。 */
    Rotation inverse(const Rotation& value)
    {
        return {value[0], -value[1], -value[2], -value[3]};
    }

    /** 最短三维旋转差，单位 deg；不直接相减欧拉角。 */
    double errorDeg(const Rotation& left, const Rotation& right)
    {
        const Rotation delta = normalized(multiply(inverse(left), right));
        return 2.0 * std::atan2(std::hypot(delta[1], delta[2], delta[3]), std::abs(delta[0])) * kDegrees;
    }

    /** 从一帧取传感器到世界的旋转。 */
    Rotation sensorRotation(const MicroduckHeadInput& frame)
    {
        return {frame.sensorToWorldWxyz[0], frame.sensorToWorldWxyz[1],
                frame.sensorToWorldWxyz[2], frame.sensorToWorldWxyz[3]};
    }

    /** 固定头部模型在实测端点的 sensor→body；速度置零只用于几何，不参与积分。 */
    Rotation modelRotation(const Endpoint& point, const Rotation& mount)
    {
        Rotation result{};
        const double still[4] = {};
        double relative[3];
        if (microduck_head_kinematics(point.angles.data(), still, mount.data(), result.data(), relative) != 0)
        {
            throw std::runtime_error("端点关节位置超出模型范围，未完成核验");
        }
        return result;
    }

    /** 对至少 0.6 s 的真实停稳窗口求均值；运动中的短暂停顿可被识别但不会单独推进阶段。 */
    bool settledEndpoint(const std::deque<MicroduckHeadInput>& frames,
                         std::size_t begin,
                         std::size_t end,
                         Endpoint& point)
    {
        point = {};
        point.begin = begin;
        point.end = end;
        point.middle = (begin + end) / 2;
        Rotation sum{};
        const Rotation reference = sensorRotation(frames[begin]);
        std::array<double, 4> minimum;
        std::array<double, 4> maximum;
        std::copy_n(frames[begin].jointAnglesRad, 4, minimum.begin());
        maximum = minimum;
        double gyroSquared = 0.0;
        for (std::size_t index = begin; index <= end; ++index)
        {
            const auto& frame = frames[index];
            const Rotation sensor = sensorRotation(frame);
            if (errorDeg(reference, sensor) > kSettledPoseSpreadDeg)
            {
                return false;
            }
            double product = 0.0;
            for (int axis = 0; axis < 4; ++axis)
            {
                product += sensor[axis] * reference[axis];
                minimum[axis] = std::min(minimum[axis], frame.jointAnglesRad[axis]);
                maximum[axis] = std::max(maximum[axis], frame.jointAnglesRad[axis]);
                if ((maximum[axis] - minimum[axis]) * kDegrees > kSettledJointSpreadDeg)
                {
                    return false;
                }
                point.angles[axis] += frame.jointAnglesRad[axis];
            }
            for (int axis = 0; axis < 4; ++axis)
            {
                sum[axis] += (product < 0.0 ? -1.0 : 1.0) * sensor[axis];
            }
            for (int axis = 0; axis < 3; ++axis)
            {
                point.bias[axis] += frame.sensorGyroRadS[axis];
                gyroSquared += frame.sensorGyroRadS[axis] * frame.sensorGyroRadS[axis];
            }
        }
        const double count = static_cast<double>(end - begin + 1);
        if (std::sqrt(gyroSquared / count) > kSettledGyroRmsRadS)
        {
            return false;
        }
        point.sensor = normalized(sum);
        for (double& value : point.angles)
        {
            value /= count;
        }
        for (double& value : point.bias)
        {
            value /= count;
        }
        return true;
    }

    /** 找出全段内实际停稳的窗口；必须覆盖起点和末点，不能用运动中途的停顿冒充两端。 */
    std::array<Endpoint, 4> findEndpoints(const std::deque<MicroduckHeadInput>& frames, int active)
    {
        std::vector<Endpoint> candidates;
        std::size_t begin = 0;
        for (std::size_t end = 0; end < frames.size(); ++end)
        {
            while (begin < end && frames[end].imuSampleNs - frames[begin + 1].imuSampleNs >= kSettledNs)
            {
                ++begin;
            }
            Endpoint point;
            if (end - begin >= 20 && frames[end].imuSampleNs - frames[begin].imuSampleNs >= kSettledNs &&
                settledEndpoint(frames, begin, end, point))
            {
                candidates.push_back(point);
            }
        }
        if (candidates.empty() || candidates.back().end != frames.size() - 1)
        {
            throw std::runtime_error("未采到完整停稳起点和终点，请保持桌面站姿并重试当前关节");
        }
        const auto low = std::min_element(candidates.begin(), candidates.end(),
            [active](const Endpoint& a, const Endpoint& b) { return a.angles[active] < b.angles[active]; });
        const auto high = std::max_element(candidates.begin(), candidates.end(),
            [active](const Endpoint& a, const Endpoint& b) { return a.angles[active] < b.angles[active]; });
        Endpoint first = *low;
        Endpoint second = *high;
        if (first.middle > second.middle)
        {
            std::swap(first, second);
        }
        return {candidates.front(), first, second, candidates.back()};
    }

    /** 按源时间积分一段完整单向旋转；扣除两端静止偏置，不要求编码器与陀螺瞬时速度相同。 */
    Rotation integrateLeg(const std::deque<MicroduckHeadInput>& frames, const Endpoint& from, const Endpoint& to)
    {
        Rotation rotation{1.0, 0.0, 0.0, 0.0};
        for (std::size_t index = from.middle + 1; index <= to.middle; ++index)
        {
            const auto& previous = frames[index - 1];
            const auto& current = frames[index];
            const double seconds = (current.imuSampleNs - previous.imuSampleNs) * 1e-9;
            Vector increment{};
            for (int axis = 0; axis < 3; ++axis)
            {
                const double bias = (from.bias[axis] + to.bias[axis]) * 0.5;
                increment[axis] = ((previous.sensorGyroRadS[axis] + current.sensorGyroRadS[axis]) * 0.5 - bias) * seconds;
            }
            const double magnitude = std::hypot(increment[0], increment[1], increment[2]);
            Rotation delta{1.0, 0.0, 0.0, 0.0};
            if (magnitude > 1e-12)
            {
                const double scale = std::sin(magnitude * 0.5) / magnitude;
                delta = {std::cos(magnitude * 0.5), increment[0] * scale, increment[1] * scale, increment[2] * scale};
            }
            rotation = normalized(multiply(rotation, delta));
        }
        return rotation;
    }
}

/** 先填真实行程和诊断量，再按四端点及三段积分核验；失败保留可解释的已测指标。 */
void microduck::verifyHeadEndpoints(const std::deque<MicroduckHeadInput>& frames,
                                   const std::array<double, 4>& mount,
                                   MicroduckHeadCalibrationStatus& status)
{
    const int active = static_cast<int>(status.nextStep) - 6;
    std::array<double, 4> minimum;
    std::array<double, 4> maximum;
    std::copy_n(frames.front().jointAnglesRad, 4, minimum.begin());
    maximum = minimum;
    double gyroSquared = 0.0;
    status.bodyGyroPeakRadS = 0.0;
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        const auto& frame = frames[index];
        if (index > 0 && frame.imuSampleNs - frames[index - 1].imuSampleNs > kMaximumGapNs)
        {
            throw std::runtime_error("运动期间采样缺口超过 60 ms，不能核对累计转角，请重试当前关节");
        }
        double squared = 0.0;
        MicroduckHeadInput input = frame;
        std::copy(mount.begin(), mount.end(), input.sensorToSiteWxyz);
        MicroduckBodyEstimate estimate{};
        const int code = microduck_estimate_body(&input, &estimate);
        if ((code != MICRODUCK_ESTIMATE_OK && code != MICRODUCK_ESTIMATE_UNVERIFIED) || estimate.estimateAvailable != 1)
        {
            throw std::runtime_error("头部测试采样无效，请重新采集当前关节");
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            squared += estimate.bodyGyroRadS[axis] * estimate.bodyGyroRadS[axis];
        }
        gyroSquared += squared;
        status.bodyGyroPeakRadS = std::max(status.bodyGyroPeakRadS, std::sqrt(squared));
        for (int joint = 0; joint < 4; ++joint)
        {
            minimum[joint] = std::min(minimum[joint], frame.jointAnglesRad[joint]);
            maximum[joint] = std::max(maximum[joint], frame.jointAnglesRad[joint]);
        }
    }
    status.bodyGyroRmsRadS = std::sqrt(gyroSquared / frames.size());
    status.headSpanDeg = (maximum[active] - minimum[active]) * kDegrees;
    if (status.headSpanDeg < 8.0)
    {
        throw std::runtime_error("实测关节总行程不足 8°，请检查跟随和机械干涉后重试");
    }
    for (int joint = 0; joint < 4; ++joint)
    {
        if (joint != active && (maximum[joint] - minimum[joint]) * kDegrees > 3.0)
        {
            throw std::runtime_error("其他头颈关节变化超过 3°，请保持站姿后重试当前关节");
        }
    }
    const auto points = findEndpoints(frames, active);
    const Rotation firstBody = multiply(points[0].sensor, inverse(modelRotation(points[0], mount)));
    double maximumEndpointErrorDeg = 0.0;
    double maximumIntegralRatio = 0.0;
    for (std::size_t index = 1; index < points.size(); ++index)
    {
        const auto& from = points[index - 1];
        const auto& to = points[index];
        const double travelDeg = std::abs(to.angles[active] - from.angles[active]) * kDegrees;
        if (to.begin <= from.end || travelDeg < kMinimumLegDeg)
        {
            throw std::runtime_error("四个独立端点或单向行程不足，不能用中途卡顿代替端点，请重试");
        }
        const Rotation currentBody = multiply(to.sensor, inverse(modelRotation(to, mount)));
        maximumEndpointErrorDeg = std::max(maximumEndpointErrorDeg, errorDeg(firstBody, currentBody));
        status.maxBodyErrorDeg = maximumEndpointErrorDeg;
        const Rotation expected = multiply(inverse(modelRotation(from, mount)), modelRotation(to, mount));
        const double expectedDeg = errorDeg({1.0, 0.0, 0.0, 0.0}, expected);
        const double integralErrorDeg = errorDeg(expected, integrateLeg(frames, from, to));
        maximumIntegralRatio = std::max(maximumIntegralRatio, integralErrorDeg / expectedDeg);
        status.gyroRelativeErrorRatio = maximumIntegralRatio;
    }
    const double positive = points[1].angles[active] - points[0].angles[active];
    const double negative = points[2].angles[active] - points[0].angles[active];
    if (positive * negative >= 0.0 || std::abs(points[3].angles[active] - points[0].angles[active]) * kDegrees > 1.0)
    {
        throw std::runtime_error("端点没有分居中位两侧或最终未回中，请检查实际关节位置后重试");
    }
    if (status.maxBodyErrorDeg > kMaximumEndpointErrorDeg)
    {
        throw std::runtime_error("停稳端点的机身姿态差超过 3°，请检查机身是否移动及头部方向");
    }
    if (status.gyroRelativeErrorRatio > kMaximumIntegralRatio)
    {
        throw std::runtime_error("单向累计转角与关节实测旋转不一致，请检查陀螺方向或采样后重试");
    }
}
