#include "head_body_estimator.h"

// 不依赖 C++ 标准库，便于在 1 GB ARM64 主控上通过稳定 C ABI 调用。
extern "C" double sin(double value);
extern "C" double cos(double value);
extern "C" double sqrt(double value);

static_assert(sizeof(unsigned int) == 4, "ABI requires uint32");
static_assert(sizeof(unsigned long long) == 8, "ABI requires uint64");
static_assert(sizeof(double) == 8, "ABI requires float64");
static_assert(sizeof(MicroduckHeadInput) == 200, "Unexpected input ABI layout");
static_assert(sizeof(MicroduckBodyEstimate) == 152, "Unexpected output ABI layout");
static_assert(sizeof(MicroduckEncoderPair) == 80, "Unexpected encoder ABI layout");

namespace
{
    constexpr unsigned long long kMaximumAgeNs = 50000000;
    constexpr unsigned long long kMaximumSkewNs = 25000000;
    constexpr double kMaximumRateRadS = 35.0;

    // 来源：冻结运行时 eecac187... 的 kinematics/assets/alpha/robot_walk.xml。
    // 每节旋转为父节点静态旋转 * 绕该节点局部 Z 轴的关节旋转。
    constexpr double kRestRotations[4][4] = {
        {0.0, 0.0, -0.7071067811865475, 0.7071067811865475},
        {0.0, 1.0, 0.0, 0.0},
        {0.0, 0.0, -0.7071067811865475, -0.7071067811865475},
        {0.7071067811865475, 0.0, -0.7071067811865475, 0.0}
    };
    constexpr double kMinimumAnglesRad[4] = {-1.5707963267948966, -1.5707963267948966,
        -2.967059728390364, -0.4363323129986037};
    constexpr double kMaximumAnglesRad[4] = {1.0471975511965976, 1.5707963267948966,
        2.967059728390357, 0.43633231299856107};

    /** 内部标量优先四元数；表示把右侧坐标系中的向量旋转到左侧坐标系。 */
    struct Quaternion
    {
        double w;
        double x;
        double y;
        double z;
    };

    /** 检查数组为有限数值；未知值不得隐式替换为零。 */
    bool isFiniteVector(const double* values, int count)
    {
        if (values == nullptr)
        {
            return false;
        }
        for (int index = 0; index < count; ++index)
        {
            if (!__builtin_isfinite(values[index]))
            {
                return false;
            }
        }
        return true;
    }

    /** 验证并归一化四元数，拒绝零四元数和明显错误的单位。 */
    bool readQuaternion(const double values[4], Quaternion& result)
    {
        if (!isFiniteVector(values, 4))
        {
            return false;
        }
        double squaredNorm = 0.0;
        for (int index = 0; index < 4; ++index)
        {
            squaredNorm += values[index] * values[index];
        }
        if (squaredNorm < 0.81 || squaredNorm > 1.21)
        {
            return false;
        }
        const double inverseNorm = 1.0 / sqrt(squaredNorm);
        result = {values[0] * inverseNorm, values[1] * inverseNorm,
            values[2] * inverseNorm, values[3] * inverseNorm};
        return true;
    }

    /** 按 Hamilton 约定组合旋转，先应用 right，再应用 left。 */
    Quaternion multiply(Quaternion left, Quaternion right)
    {
        return {
            left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
            left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
            left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
            left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w
        };
    }

    /** 返回单位四元数的逆旋转。 */
    Quaternion conjugate(Quaternion value)
    {
        return {value.w, -value.x, -value.y, -value.z};
    }

    /** 用单位四元数旋转三维向量；允许输入和输出指向相同数组。 */
    void rotate(Quaternion rotation, const double input[3], double output[3])
    {
        const Quaternion vector = {0.0, input[0], input[1], input[2]};
        const Quaternion rotated = multiply(multiply(rotation, vector), conjugate(rotation));
        output[0] = rotated.x;
        output[1] = rotated.y;
        output[2] = rotated.z;
    }

    /** 写出标量优先四元数；统一正半球以降低可视化符号跳变。 */
    void writeQuaternion(Quaternion value, double output[4])
    {
        const double sign = value.w < 0.0 ? -1.0 : 1.0;
        output[0] = value.w * sign;
        output[1] = value.x * sign;
        output[2] = value.y * sign;
        output[3] = value.z * sign;
    }

    /** 初始化失败输出：缺失或错误姿态必须是 NaN，而非看似有效的直立四元数。 */
    void clearEstimate(MicroduckBodyEstimate& output)
    {
        output.abiVersion = 1;
        output.status = MICRODUCK_ESTIMATE_INVALID;
        output.estimateAvailable = 0;
        output.motionAuthorized = 0;
        const double invalid = __builtin_nan("");
        for (int index = 0; index < 4; ++index)
        {
            output.bodyToWorldWxyz[index] = invalid;
            output.sensorToBodyWxyz[index] = invalid;
        }
        for (int index = 0; index < 3; ++index)
        {
            output.bodyGyroRadS[index] = invalid;
            output.gravityInBody[index] = invalid;
            output.headRelativeGyroInBodyRadS[index] = invalid;
        }
    }

    /** 检查同一时钟域中的测量时刻，包含未来时间、老数据和不同步数据。 */
    int validateTimes(const MicroduckHeadInput& input)
    {
        const unsigned long long times[3] = {input.imuSampleNs, input.jointSampleNs, input.velocitySampleNs};
        unsigned long long oldest = input.nowNs;
        unsigned long long newest = 0;
        for (int index = 0; index < 3; ++index)
        {
            if (times[index] == 0 || times[index] > input.nowNs)
            {
                return MICRODUCK_ESTIMATE_INVALID;
            }
            if (input.nowNs - times[index] > kMaximumAgeNs)
            {
                return MICRODUCK_ESTIMATE_STALE;
            }
            if (times[index] < oldest)
            {
                oldest = times[index];
            }
            if (times[index] > newest)
            {
                newest = times[index];
            }
        }
        return newest - oldest > kMaximumSkewNs ? MICRODUCK_ESTIMATE_UNSYNCHRONIZED : MICRODUCK_ESTIMATE_OK;
    }
} // namespace

/** 实现头部正运动学与角速度投影；单位、输出约定见对应头文件。 */
int microduck_head_kinematics(const double anglesRad[4], const double velocityRadS[4],
    const double sensorToSiteWxyz[4], double sensorToBodyWxyz[4], double relativeGyroRadS[3])
{
    if (sensorToBodyWxyz == nullptr || relativeGyroRadS == nullptr)
    {
        return MICRODUCK_ESTIMATE_INVALID;
    }
    for (int index = 0; index < 4; ++index)
    {
        sensorToBodyWxyz[index] = __builtin_nan("");
    }
    for (int index = 0; index < 3; ++index)
    {
        relativeGyroRadS[index] = __builtin_nan("");
    }
    Quaternion sensorToSite;
    if (!isFiniteVector(anglesRad, 4) || !isFiniteVector(velocityRadS, 4)
        || !readQuaternion(sensorToSiteWxyz, sensorToSite))
    {
        return MICRODUCK_ESTIMATE_INVALID;
    }
    Quaternion accumulated = {1.0, 0.0, 0.0, 0.0};
    double relativeVelocity[3] = {0.0, 0.0, 0.0};
    for (int joint = 0; joint < 4; ++joint)
    {
        if (anglesRad[joint] < kMinimumAnglesRad[joint] || anglesRad[joint] > kMaximumAnglesRad[joint])
        {
            return MICRODUCK_ESTIMATE_JOINT_LIMIT;
        }
        if (__builtin_fabs(velocityRadS[joint]) > kMaximumRateRadS)
        {
            return MICRODUCK_ESTIMATE_INVALID;
        }
        const Quaternion rest = {kRestRotations[joint][0], kRestRotations[joint][1],
            kRestRotations[joint][2], kRestRotations[joint][3]};
        accumulated = multiply(accumulated, rest);
        const double localAxis[3] = {0.0, 0.0, 1.0};
        double axisInBody[3];
        rotate(accumulated, localAxis, axisInBody);
        for (int axis = 0; axis < 3; ++axis)
        {
            relativeVelocity[axis] += axisInBody[axis] * velocityRadS[joint];
        }
        const double halfAngle = anglesRad[joint] * 0.5;
        const Quaternion hinge = {cos(halfAngle), 0.0, 0.0, sin(halfAngle)};
        accumulated = multiply(accumulated, hinge);
    }
    writeQuaternion(multiply(accumulated, sensorToSite), sensorToBodyWxyz);
    for (int axis = 0; axis < 3; ++axis)
    {
        relativeGyroRadS[axis] = relativeVelocity[axis];
    }
    return MICRODUCK_ESTIMATE_OK;
}

/** 按 ABI v1 校验输入并估计躯干状态；失败不输出有效姿态。 */
int microduck_estimate_body(const MicroduckHeadInput* input, MicroduckBodyEstimate* output)
{
    if (output == nullptr)
    {
        return MICRODUCK_ESTIMATE_INVALID;
    }
    clearEstimate(*output);
    if (input == nullptr || input->abiVersion != 1 || input->reserved != 0
        || input->mountVerified > 1 || input->jointsVerified > 1)
    {
        return output->status;
    }
    const int timeStatus = validateTimes(*input);
    if (timeStatus != MICRODUCK_ESTIMATE_OK)
    {
        output->status = timeStatus;
        return output->status;
    }
    Quaternion sensorToWorld;
    if (!readQuaternion(input->sensorToWorldWxyz, sensorToWorld)
        || !isFiniteVector(input->sensorGyroRadS, 3))
    {
        return output->status;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        if (__builtin_fabs(input->sensorGyroRadS[axis]) > kMaximumRateRadS)
        {
            return output->status;
        }
    }
    double sensorToBodyValues[4];
    double relativeGyro[3];
    const int chainStatus = microduck_head_kinematics(input->jointAnglesRad, input->jointVelocityRadS,
        input->sensorToSiteWxyz, sensorToBodyValues, relativeGyro);
    if (chainStatus != MICRODUCK_ESTIMATE_OK)
    {
        output->status = chainStatus;
        return output->status;
    }
    Quaternion sensorToBody;
    if (!readQuaternion(sensorToBodyValues, sensorToBody))
    {
        return output->status;
    }
    // q_WB = q_WS * inverse(q_BS)。先去除转头的相对旋转，再得到躯干姿态。
    const Quaternion bodyToWorld = multiply(sensorToWorld, conjugate(sensorToBody));
    writeQuaternion(bodyToWorld, output->bodyToWorldWxyz);
    writeQuaternion(sensorToBody, output->sensorToBodyWxyz);
    rotate(sensorToBody, input->sensorGyroRadS, output->bodyGyroRadS);
    for (int axis = 0; axis < 3; ++axis)
    {
        // ω_body = R_BS * ω_sensor - J_angular(q) * qdot；不能直接旋转陀螺仪向量。
        output->headRelativeGyroInBodyRadS[axis] = relativeGyro[axis];
        output->bodyGyroRadS[axis] -= relativeGyro[axis];
    }
    const double worldGravity[3] = {0.0, 0.0, -1.0};
    rotate(conjugate(bodyToWorld), worldGravity, output->gravityInBody);
    output->estimateAvailable = 1;
    output->status = input->mountVerified && input->jointsVerified
        ? MICRODUCK_ESTIMATE_OK : MICRODUCK_ESTIMATE_UNVERIFIED;
    return output->status;
}

/** 对齐实际编码器与 IMU 源时间；只插值，不外推。 */
int microduck_estimate_encoders(const MicroduckHeadInput* sensorInput,
    const MicroduckEncoderPair* encoders, MicroduckBodyEstimate* output)
{
    if (output == nullptr)
    {
        return MICRODUCK_ESTIMATE_INVALID;
    }
    clearEstimate(*output);
    if (sensorInput == nullptr || encoders == nullptr || encoders->previousNs == 0
        || encoders->currentNs <= encoders->previousNs || encoders->currentNs > sensorInput->nowNs)
    {
        return output->status;
    }
    const unsigned long long intervalNs = encoders->currentNs - encoders->previousNs;
    if (intervalNs < 5000000 || intervalNs > 40000000
        || sensorInput->imuSampleNs < encoders->previousNs || sensorInput->imuSampleNs > encoders->currentNs)
    {
        output->status = MICRODUCK_ESTIMATE_UNSYNCHRONIZED;
        return output->status;
    }
    if (sensorInput->nowNs - encoders->previousNs > kMaximumAgeNs)
    {
        output->status = MICRODUCK_ESTIMATE_STALE;
        return output->status;
    }
    MicroduckHeadInput aligned = *sensorInput;
    aligned.jointSampleNs = sensorInput->imuSampleNs;
    aligned.velocitySampleNs = encoders->previousNs + intervalNs / 2;
    const double fraction = static_cast<double>(sensorInput->imuSampleNs - encoders->previousNs) / intervalNs;
    constexpr double kRadiansPerCount = 6.28318530717958647692 / 4096.0;
    const double intervalSeconds = intervalNs * 1e-9;
    for (int joint = 0; joint < 4; ++joint)
    {
        if ((encoders->direction[joint] != -1 && encoders->direction[joint] != 1)
            || encoders->previousRaw[joint] < 0 || encoders->previousRaw[joint] > 4095
            || encoders->currentRaw[joint] < 0 || encoders->currentRaw[joint] > 4095
            || encoders->zeroRaw[joint] < 0 || encoders->zeroRaw[joint] > 4095)
        {
            return output->status;
        }
        const double differenceCounts = encoders->currentRaw[joint] - encoders->previousRaw[joint];
        const double positionCounts = encoders->previousRaw[joint] + fraction * differenceCounts;
        const double scale = encoders->direction[joint] * kRadiansPerCount;
        aligned.jointAnglesRad[joint] = (positionCounts - encoders->zeroRaw[joint]) * scale;
        aligned.jointVelocityRadS[joint] = differenceCounts * scale / intervalSeconds;
    }
    return microduck_estimate_body(&aligned, output);
}
