#include "head_calibration.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <new>
#include <stdexcept>

static_assert(sizeof(MicroduckHeadCalibrationStatus) == 344, "Unexpected calibration ABI layout");

namespace
{
    using Vector = std::array<double, 3>;
    using Rotation = std::array<double, 4>;
    constexpr double kDegrees = 57.29577951308232;
    constexpr unsigned long long kStaticWindowNs = 2000000000;
    constexpr unsigned long long kMotionWindowNs = 6000000000;
    constexpr unsigned long long kMaximumGapNs = 100000000;
    constexpr unsigned int kMinimumSamples = 60;
    constexpr double kStaticSpreadDeg = 1.5;
    constexpr double kStaticHeadDeg = 2.0;
    constexpr double kMinimumHeadSpanDeg = 8.0;
    constexpr double kMaximumBodyErrorDeg = 5.0;
    constexpr double kStaticGyroRadS = 0.08;
    constexpr double kMotionGyroRmsRadS = 0.15;
    constexpr double kMotionGyroPeakRadS = 0.5;

    /** 单个已对齐的有效传感器及编码器帧；时间为 IMU 源时间 ns。 */
    struct Frame
    {
        MicroduckHeadInput aligned;
    };

    /** 仅本次采集有效的几何状态；不保存文件，也不接触电机。 */
    struct Session
    {
        std::deque<Frame> frames;
        unsigned int nextStep = 0;
        unsigned long long lastNs = 0;
        std::array<Vector, 3> fitGravity{};
        std::array<double, 4> referenceAngles{};
        Rotation mount{1.0, 0.0, 0.0, 0.0};
        Vector gravityReference{};
    };

    /** 计算三个轴的点积，用于单位向量及相对倾斜判定。 */
    double dot(const Vector& left, const Vector& right)
    {
        return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
    }

    /** 计算右手叉积；上 × 前 得到左。 */
    Vector cross(const Vector& left, const Vector& right)
    {
        return {left[1] * right[2] - left[2] * right[1],
                left[2] * right[0] - left[0] * right[2],
                left[0] * right[1] - left[1] * right[0]};
    }

    /** 归一化实测方向；退化输入拒绝，不选择任意轴掩盖无效采样。 */
    Vector unit(Vector value)
    {
        const double norm = std::sqrt(dot(value, value));
        if (!std::isfinite(norm) || norm < 1e-8)
        {
            throw std::runtime_error("采样方向退化，请重新采集有明显倾斜的姿态");
        }
        for (double& item : value)
        {
            item /= norm;
        }
        return value;
    }

    /** 两个实测重力单位方向之间的夹角，单位 deg。 */
    double angle(const Vector& left, const Vector& right)
    {
        return std::acos(std::clamp(dot(unit(left), unit(right)), -1.0, 1.0)) * kDegrees;
    }

    /** Hamilton 四元数乘法，先应用 right，再应用 left。 */
    Rotation multiply(const Rotation& left, const Rotation& right)
    {
        const auto& a = left;
        const auto& b = right;
        return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
                a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
                a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
                a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
    }

    /** 单位四元数的逆。 */
    Rotation inverse(Rotation value)
    {
        return {value[0], -value[1], -value[2], -value[3]};
    }

    /** 计算两个姿态的最短旋转差，允许四元数正负号等价。 */
    double rotationError(const Rotation& left, const Rotation& right)
    {
        double product = 0.0;
        double leftNorm = 0.0;
        double rightNorm = 0.0;
        for (int index = 0; index < 4; ++index)
        {
            product += left[index] * right[index];
            leftNorm += left[index] * left[index];
            rightNorm += right[index] * right[index];
        }
        return 2.0 * std::acos(std::clamp(std::abs(product) / std::sqrt(leftNorm * rightNorm), 0.0, 1.0)) * kDegrees;
    }

    /** 从机身/传感器到世界的单位四元数取本地重力方向。 */
    Vector gravity(const double* values)
    {
        double norm = 0.0;
        for (int index = 0; index < 4; ++index)
        {
            norm += values[index] * values[index];
        }
        const double w = values[0] / std::sqrt(norm);
        const double x = values[1] / std::sqrt(norm);
        const double y = values[2] / std::sqrt(norm);
        const double z = values[3] / std::sqrt(norm);
        return {2.0 * (w * y - x * z), -2.0 * (y * z + w * x), -1.0 + 2.0 * (x * x + y * y)};
    }

    /** 将“上”和一个倾斜方向正交化，得到该方向在水平面里的单位向量。 */
    Vector horizontal(const Vector& direction, const Vector& up)
    {
        const double vertical = dot(direction, up);
        return unit(
            {direction[0] - vertical * up[0], direction[1] - vertical * up[1], direction[2] - vertical * up[2]});
    }

    /** 从三个正交行向量恢复单位旋转四元数，不改变安装方向的坐标约定。 */
    Rotation fromRows(const std::array<Vector, 3>& matrix)
    {
        Rotation result{};
        const double trace = matrix[0][0] + matrix[1][1] + matrix[2][2];
        if (trace > 0.0)
        {
            const double scale = std::sqrt(trace + 1.0) * 2.0;
            result = {scale / 4.0,
                      (matrix[2][1] - matrix[1][2]) / scale,
                      (matrix[0][2] - matrix[2][0]) / scale,
                      (matrix[1][0] - matrix[0][1]) / scale};
        }
        else
        {
            int axis = 0;
            for (int index = 1; index < 3; ++index)
            {
                if (matrix[index][index] > matrix[axis][axis])
                {
                    axis = index;
                }
            }
            const int next = (axis + 1) % 3;
            const int last = (axis + 2) % 3;
            const double scale = std::sqrt(1.0 + matrix[axis][axis] - matrix[next][next] - matrix[last][last]) * 2.0;
            result[0] = (matrix[last][next] - matrix[next][last]) / scale;
            result[axis + 1] = scale / 4.0;
            result[next + 1] = (matrix[axis][next] + matrix[next][axis]) / scale;
            result[last + 1] = (matrix[axis][last] + matrix[last][axis]) / scale;
        }
        return result;
    }

    /** 取当前步骤的有界新鲜窗口；每次成功后清空，防止复用上一步的采样。 */
    std::deque<Frame> window(const Session& session)
    {
        std::deque<Frame> selected;
        const bool moving = session.nextStep >= 4 && session.nextStep <= 7;
        const auto span = moving ? kMotionWindowNs : kStaticWindowNs;
        for (const Frame& frame : session.frames)
        {
            if (session.lastNs - frame.aligned.imuSampleNs <= span)
            {
                selected.push_back(frame);
            }
        }
        return selected;
    }

    /** 汇总窗口大小和时间；少量帧或过短采集不能作为核验依据。 */
    void requireWindow(const std::deque<Frame>& frames, MicroduckHeadCalibrationStatus& status)
    {
        status.sampleCount = static_cast<unsigned int>(frames.size());
        status.sampleSpanS =
            frames.empty() ? 0.0 : (frames.back().aligned.imuSampleNs - frames.front().aligned.imuSampleNs) * 1e-9;
        if (status.sampleCount < kMinimumSamples || status.sampleSpanS < 1.5)
        {
            throw std::runtime_error("新鲜采样不足，请保持当前姿态至少 2 秒后重试");
        }
    }

    /** 计算静态段的平均传感器重力，并检查头颈保持不变和静止质量。 */
    Vector staticGravity(const Session& session, const std::deque<Frame>& frames)
    {
        Vector sum{};
        for (const Frame& frame : frames)
        {
            const Vector value = gravity(frame.aligned.sensorToWorldWxyz);
            for (int axis = 0; axis < 3; ++axis)
            {
                sum[axis] += value[axis];
            }
            if (dot({frame.aligned.sensorGyroRadS[0], frame.aligned.sensorGyroRadS[1], frame.aligned.sensorGyroRadS[2]},
                    {frame.aligned.sensorGyroRadS[0],
                     frame.aligned.sensorGyroRadS[1],
                     frame.aligned.sensorGyroRadS[2]}) > kStaticGyroRadS * kStaticGyroRadS)
            {
                throw std::runtime_error("还在转动，请停稳后保持 2 秒再采集");
            }
            for (int joint = 0; joint < 4; ++joint)
            {
                const double reference = session.nextStep == 0 ? frames.front().aligned.jointAnglesRad[joint]
                                                               : session.referenceAngles[joint];
                if (std::abs(frame.aligned.jointAnglesRad[joint] - reference) * kDegrees > kStaticHeadDeg)
                {
                    throw std::runtime_error("求安装方向时头颈必须保持初始姿态，仅倾斜整个机身");
                }
            }
        }
        const Vector mean = unit(sum);
        for (const Frame& frame : frames)
        {
            if (angle(mean, gravity(frame.aligned.sensorToWorldWxyz)) > kStaticSpreadDeg)
            {
                throw std::runtime_error("静止段晃动超过 1.5 度，请停稳后重试");
            }
        }
        return mean;
    }

    /** 从机身摆正、前倾、左倾三个独立方向求传感器→头部安装旋转。 */
    void solveMount(Session& session)
    {
        const Vector down = session.fitGravity[0];
        const Vector up{-down[0], -down[1], -down[2]};
        const Vector forward = horizontal(session.fitGravity[1], up);
        const Vector expectedLeft = unit(cross(up, forward));
        const Vector measuredLeft = horizontal(session.fitGravity[2], up);
        if (angle(expectedLeft, measuredLeft) > 12.0)
        {
            throw std::runtime_error("前倾和左倾方向不一致，请重新开始并核对鸭子自己的左侧");
        }
        const Rotation sensorToBody = fromRows({forward, expectedLeft, up});
        const double identity[4] = {1.0, 0.0, 0.0, 0.0};
        const double still[4] = {};
        Rotation siteToBody{};
        double relative[3];
        if (microduck_head_kinematics(session.referenceAngles.data(), still, identity, siteToBody.data(), relative) !=
            MICRODUCK_ESTIMATE_OK)
        {
            throw std::runtime_error("初始头颈姿态超出模型范围，请承托头部后重新开始");
        }
        session.mount = multiply(inverse(siteToBody), sensorToBody);
    }

    /** 用候选安装重算每帧躯干状态，未核验标志只影响状态码，不伪造通过。 */
    MicroduckBodyEstimate body(const Session& session, const Frame& frame)
    {
        MicroduckHeadInput input = frame.aligned;
        std::copy(session.mount.begin(), session.mount.end(), input.sensorToSiteWxyz);
        MicroduckBodyEstimate estimate{};
        const int code = microduck_estimate_body(&input, &estimate);
        if ((code != MICRODUCK_ESTIMATE_OK && code != MICRODUCK_ESTIMATE_UNVERIFIED) ||
            estimate.estimateAvailable != 1 || estimate.motionAuthorized != 0)
        {
            throw std::runtime_error("候选安装补偿失败，请核对采样时效、关节范围和数据质量");
        }
        return estimate;
    }

    /** 核验固定躯干时的头部独立运动及角速度消除，或整机倾斜的坐标方向。 */
    void verify(Session& session, const std::deque<Frame>& frames, MicroduckHeadCalibrationStatus& status)
    {
        std::array<double, 4> minimum;
        std::array<double, 4> maximum;
        std::copy(std::begin(frames.front().aligned.jointAnglesRad),
                  std::end(frames.front().aligned.jointAnglesRad),
                  minimum.begin());
        maximum = minimum;
        double gyroSquared = 0.0;
        double gyroPeak = 0.0;
        Vector gravitySum{};
        const bool moving = session.nextStep >= 4 && session.nextStep <= 7;
        const MicroduckBodyEstimate first = body(session, frames.front());
        const Rotation firstRotation{
            first.bodyToWorldWxyz[0], first.bodyToWorldWxyz[1], first.bodyToWorldWxyz[2], first.bodyToWorldWxyz[3]};
        // 六轴 IMU 没有绝对航向：只判本窗口的躯干旋转变化，不把步骤间的航向漂移当安装变化。
        for (const Frame& frame : frames)
        {
            const auto estimate = body(session, frame);
            const Rotation rotation{estimate.bodyToWorldWxyz[0],
                                    estimate.bodyToWorldWxyz[1],
                                    estimate.bodyToWorldWxyz[2],
                                    estimate.bodyToWorldWxyz[3]};
            status.maxBodyErrorDeg = std::max(status.maxBodyErrorDeg, rotationError(rotation, firstRotation));
            double squared = 0.0;
            for (int axis = 0; axis < 3; ++axis)
            {
                squared += estimate.bodyGyroRadS[axis] * estimate.bodyGyroRadS[axis];
                gravitySum[axis] += estimate.gravityInBody[axis];
            }
            gyroSquared += squared;
            gyroPeak = std::max(gyroPeak, std::sqrt(squared));
            for (int joint = 0; joint < 4; ++joint)
            {
                minimum[joint] = std::min(minimum[joint], frame.aligned.jointAnglesRad[joint]);
                maximum[joint] = std::max(maximum[joint], frame.aligned.jointAnglesRad[joint]);
            }
        }
        status.bodyGyroRmsRadS = std::sqrt(gyroSquared / frames.size());
        status.bodyGyroPeakRadS = gyroPeak;
        const Vector meanGravity = unit(gravitySum);
        if (moving)
        {
            const int active = static_cast<int>(session.nextStep) - 4;
            status.headSpanDeg = (maximum[active] - minimum[active]) * kDegrees;
            if (status.headSpanDeg < kMinimumHeadSpanDeg)
            {
                throw std::runtime_error("该头颈关节变化不足 8 度，请固定躯干、小幅缓慢往返转动后立即点采集");
            }
            for (int joint = 0; joint < 4; ++joint)
            {
                if (joint != active && (maximum[joint] - minimum[joint]) * kDegrees > 3.0)
                {
                    throw std::runtime_error("一次只检查一个关节，其他头颈关节变化须不超过 3 度");
                }
            }
            if (status.maxBodyErrorDeg > kMaximumBodyErrorDeg || status.bodyGyroRmsRadS > kMotionGyroRmsRadS ||
                gyroPeak > kMotionGyroPeakRadS)
            {
                throw std::runtime_error("固定躯干补偿误差过大：请核对躯干固定、关节方向、安装及头部振动，不能保存");
            }
            return;
        }
        for (int joint = 0; joint < 4; ++joint)
        {
            if ((maximum[joint] - minimum[joint]) * kDegrees > kStaticHeadDeg)
            {
                throw std::runtime_error("整机姿态复核时请固定头颈相对机身，停稳后再采集");
            }
        }
        if (status.maxBodyErrorDeg > kStaticSpreadDeg || status.bodyGyroRmsRadS > kStaticGyroRadS)
        {
            throw std::runtime_error("姿态复核仍在晃动，请停稳至少 2 秒");
        }
        if (session.nextStep == 3)
        {
            if (angle(meanGravity, {0.0, 0.0, -1.0}) > 5.0)
            {
                throw std::runtime_error("机身摆正后的补偿倾斜超过 5 度，请重新开始安装校准");
            }
            session.gravityReference = meanGravity;
            return;
        }
        const double tilt = angle(meanGravity, session.gravityReference);
        const double heading =
            std::atan2(meanGravity[1] - session.gravityReference[1], meanGravity[0] - session.gravityReference[0]) *
            kDegrees;
        const double expected = session.nextStep == 8 ? 0.0 : 90.0;
        const double error = std::abs(std::remainder(heading - expected, 360.0));
        if (tilt < 12.0 || tilt > 35.0 || error > 20.0)
        {
            throw std::runtime_error("整机倾斜复核未通过：倾角须为 12–35 度，方向误差须不超过 20 度");
        }
    }

    /** 验证恰好下一步；每步成功才推进，不以按钮确认代替测量。 */
    void capture(Session& session, unsigned int step, MicroduckHeadCalibrationStatus& status)
    {
        if (step != session.nextStep || step >= 10)
        {
            throw std::runtime_error("校验步骤已改变，请按当前提示继续，不可跳步");
        }
        const auto frames = window(session);
        requireWindow(frames, status);
        if (step < 3)
        {
            const Vector measured = staticGravity(session, frames);
            if (step > 0)
            {
                const double tilt = angle(measured, session.fitGravity[0]);
                if (tilt < 12.0 || tilt > 35.0)
                {
                    throw std::runtime_error("机身倾斜须为 12–35 度，头颈保持不变，再停稳 2 秒");
                }
            }
            session.fitGravity[step] = measured;
            if (step == 0)
            {
                for (int joint = 0; joint < 4; ++joint)
                {
                    double sum = 0.0;
                    for (const Frame& frame : frames)
                    {
                        sum += frame.aligned.jointAnglesRad[joint];
                    }
                    session.referenceAngles[joint] = sum / frames.size();
                }
            }
            if (step == 2)
            {
                solveMount(session);
            }
        }
        else
        {
            verify(session, frames, status);
        }
        ++session.nextStep;
        session.frames.clear();
    }
} // namespace

/** 创建隔离诊断状态；异常不跨 C ABI。 */
void* microduck_head_calibration_create(void)
{
    try
    {
        return new (std::nothrow) Session;
    }
    catch (...)
    {
        return nullptr;
    }
}

/** 仅由创建方线程释放本次会话，不访问任何硬件。 */
void microduck_head_calibration_destroy(void* session)
{
    delete static_cast<Session*>(session);
}

/** 复用编码器对齐和估计输入验证，断流后当前窗口重新积累。 */
int microduck_head_calibration_observe(void* handle,
                                       const MicroduckHeadInput* input,
                                       const MicroduckEncoderPair* encoders)
{
    if (handle == nullptr || input == nullptr)
    {
        return MICRODUCK_ESTIMATE_INVALID;
    }
    Session& session = *static_cast<Session*>(handle);
    try
    {
        MicroduckHeadInput aligned{};
        int code = microduck_align_encoders(input, encoders, &aligned);
        if (code != MICRODUCK_ESTIMATE_OK)
        {
            session.frames.clear();
            return code;
        }
        MicroduckBodyEstimate validation{};
        code = microduck_estimate_body(&aligned, &validation);
        if ((code != MICRODUCK_ESTIMATE_OK && code != MICRODUCK_ESTIMATE_UNVERIFIED) ||
            aligned.imuSampleNs <= session.lastNs)
        {
            session.frames.clear();
            return MICRODUCK_ESTIMATE_INVALID;
        }
        if (session.lastNs != 0 && aligned.imuSampleNs - session.lastNs > kMaximumGapNs)
        {
            session.frames.clear();
        }
        session.lastNs = aligned.imuSampleNs;
        session.frames.push_back({aligned});
        while (session.frames.size() > 350 ||
               session.lastNs - session.frames.front().aligned.imuSampleNs > kMotionWindowNs)
        {
            session.frames.pop_front();
        }
        return MICRODUCK_ESTIMATE_OK;
    }
    catch (...)
    {
        session.frames.clear();
        return MICRODUCK_ESTIMATE_INVALID;
    }
}

/** 返回当前测量结果；标准库错误被转换为有界 UTF-8 文本。 */
int microduck_head_calibration_capture(void* handle,
                                       unsigned int step,
                                       unsigned int shouldCapture,
                                       MicroduckHeadCalibrationStatus* status)
{
    if (handle == nullptr || status == nullptr)
    {
        return MICRODUCK_ESTIMATE_INVALID;
    }
    *status = {};
    status->abiVersion = 1;
    Session& session = *static_cast<Session*>(handle);
    int result = MICRODUCK_ESTIMATE_OK;
    try
    {
        if (shouldCapture > 1)
        {
            throw std::runtime_error("无效的校验操作");
        }
        if (shouldCapture == 1)
        {
            capture(session, step, *status);
        }
        else
        {
            const auto frames = window(session);
            status->sampleCount = static_cast<unsigned int>(frames.size());
            status->sampleSpanS =
                frames.empty() ? 0.0 : (frames.back().aligned.imuSampleNs - frames.front().aligned.imuSampleNs) * 1e-9;
        }
    }
    catch (const std::exception& error)
    {
        std::snprintf(status->reason, sizeof(status->reason), "%s", error.what());
        result = MICRODUCK_ESTIMATE_INVALID;
    }
    catch (...)
    {
        std::snprintf(status->reason, sizeof(status->reason), "%s", "校验计算异常，未保存");
        result = MICRODUCK_ESTIMATE_INVALID;
    }
    status->nextStep = session.nextStep;
    status->mountAvailable = session.nextStep >= 3 ? 1 : 0;
    std::copy(session.mount.begin(), session.mount.end(), status->mountWxyz);
    return result;
}
