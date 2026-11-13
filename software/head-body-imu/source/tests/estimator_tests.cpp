#include "../head_body_estimator.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "mujoco_head_fixtures.inc"

using Quaternion = std::array<double, 4>;
static int assertions = 0;

/** 验证物理或协议不变量；失败立即返回非零，禁止 NDEBUG 禁用测试。 */
void require(bool condition, const char* message)
{
    ++assertions;
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

/** 测试侧组合预设的真实姿态，不调用被测程序内部的数学函数。 */
Quaternion compose(Quaternion a, Quaternion b)
{
    return {
        a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
        a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
        a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
        a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]
    };
}

/** 返回测试侧逆旋转。 */
Quaternion inverse(Quaternion value)
{
    return {value[0], -value[1], -value[2], -value[3]};
}

/** 测试侧将已知躯干角速度投到传感器坐标。 */
std::array<double, 3> rotate(Quaternion rotation, std::array<double, 3> value)
{
    const Quaternion product = compose(compose(rotation, {0, value[0], value[1], value[2]}), inverse(rotation));
    return {product[1], product[2], product[3]};
}

/** 使用 |dot| 比较四元数，正确处理 q 与 -q 的同一旋转。 */
void requireSameRotation(const double* actual, Quaternion expected, const char* message)
{
    double dot = 0;
    double normSquared = 0;
    for (int axis = 0; axis < 4; ++axis)
    {
        dot += actual[axis] * expected[axis];
        normSquared += actual[axis] * actual[axis];
    }
    require(std::fabs(std::fabs(dot) - 1.0) < 1e-9 && std::fabs(normSquared - 1.0) < 1e-9, message);
}

/** 构造同一时钟、未标定、零关节的输入；传感器姿态设为躯干直立对应姿态。 */
MicroduckHeadInput makeInput()
{
    MicroduckHeadInput input{};
    input.abiVersion = 1;
    input.nowNs = 1000000000;
    input.imuSampleNs = input.jointSampleNs = input.velocitySampleNs = input.nowNs - 1000000;
    input.sensorToSiteWxyz[0] = 1;
    double velocity[3];
    require(microduck_head_kinematics(input.jointAnglesRad, input.jointVelocityRadS,
        input.sensorToSiteWxyz, input.sensorToWorldWxyz, velocity) == MICRODUCK_ESTIMATE_OK, "zero FK");
    return input;
}

/** 用 MuJoCo 生成的 64 个独立参考姿态验证旋转次序、关节符号和安装坐标。 */
void testMujocoFixtures()
{
    for (const auto& fixture : kMujocoHeadFixtures)
    {
        double sensorRotation[4];
        double relativeVelocity[3];
        const double stationary[4] = {};
        const double mounting[4] = {1, 0, 0, 0};
        require(microduck_head_kinematics(fixture, stationary, mounting, sensorRotation,
            relativeVelocity) == MICRODUCK_ESTIMATE_OK, "MuJoCo fixture inside joint limits");
        const Quaternion sensor = {sensorRotation[0], sensorRotation[1], sensorRotation[2], sensorRotation[3]};
        const Quaternion camera = compose(sensor, {std::sqrt(0.5), 0, std::sqrt(0.5), 0});
        requireSameRotation(camera.data(), {fixture[4], fixture[5], fixture[6], fixture[7]}, "MuJoCo camera rotation");
    }
}

/** 数值微分 FK 独立校验角速度 Jacobian，并验证转头与身体旋转可以分离。 */
void testDynamicCompensation()
{
    constexpr double stepSeconds = 1e-6;
    for (int sample = 0; sample < 100; ++sample)
    {
        MicroduckHeadInput input = makeInput();
        const double phase = sample * 0.07;
        const double angles[4] = {0.3 * std::sin(phase), -0.4 * std::cos(phase),
            0.8 * std::sin(phase * 0.5), 0.2 * std::cos(phase)};
        const double rates[4] = {0.7, -0.4, 0.6, 0.3};
        const double correction[4] = {std::cos(0.2), std::sin(0.2), 0, 0};
        double futureAngles[4];
        for (int joint = 0; joint < 4; ++joint)
        {
            input.jointAnglesRad[joint] = angles[joint];
            input.jointVelocityRadS[joint] = rates[joint];
            input.sensorToSiteWxyz[joint] = correction[joint];
            futureAngles[joint] = angles[joint] + rates[joint] * stepSeconds;
        }
        double rotation[4];
        double futureRotation[4];
        double predictedRate[3];
        double unused[3];
        require(microduck_head_kinematics(angles, rates, correction, rotation, predictedRate) == 0, "dynamic FK");
        require(microduck_head_kinematics(futureAngles, rates, correction, futureRotation, unused) == 0, "future FK");
        const Quaternion current = {rotation[0], rotation[1], rotation[2], rotation[3]};
        const Quaternion future = {futureRotation[0], futureRotation[1], futureRotation[2], futureRotation[3]};
        Quaternion delta = compose(future, inverse(current));
        if (delta[0] < 0)
        {
            for (double& component : delta)
            {
                component = -component;
            }
        }
        const std::array<double, 3> bodyRate = {0.15, -0.23, 0.31};
        std::array<double, 3> headRate;
        for (int axis = 0; axis < 3; ++axis)
        {
            const double numericalRate = 2.0 * delta[axis + 1] / stepSeconds;
            require(std::fabs(numericalRate - predictedRate[axis]) < 1e-6, "angular Jacobian vs finite difference");
            headRate[axis] = numericalRate + bodyRate[axis];
        }
        const auto sensorGyro = rotate(inverse(current), headRate);
        const Quaternion body = compose({std::cos(0.15), std::sin(0.15), 0, 0},
            {std::cos(-0.23), 0, std::sin(-0.23), 0});
        const Quaternion sensorWorld = compose(body, current);
        for (int axis = 0; axis < 4; ++axis)
        {
            // 每隔一帧翻转四元数符号，验证不会引入虚假的姿态跳变。
            input.sensorToWorldWxyz[axis] = sensorWorld[axis] * (sample % 2 ? -1 : 1);
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            input.sensorGyroRadS[axis] = sensorGyro[axis];
        }
        MicroduckBodyEstimate output;
        require(microduck_estimate_body(&input, &output) == MICRODUCK_ESTIMATE_UNVERIFIED,
            "unverified mount stays preview-only");
        require(output.estimateAvailable == 1 && output.motionAuthorized == 0, "calculation is not motion authorization");
        requireSameRotation(output.bodyToWorldWxyz, body, "recover true body rotation during head motion");
        const auto expectedGravity = rotate(inverse(body), {0, 0, -1});
        for (int axis = 0; axis < 3; ++axis)
        {
            require(std::fabs(output.bodyGyroRadS[axis] - bodyRate[axis]) < 1e-6, "remove head angular velocity");
            require(std::fabs(output.gravityInBody[axis] - expectedGravity[axis]) < 1e-9, "projected body gravity");
        }
    }
}

/** 错误必须关闭估计输出，不能把未标定、陈旧或未知数据伪装为直立。 */
void testValidation()
{
    MicroduckHeadInput input = makeInput();
    MicroduckBodyEstimate output;
    require(microduck_estimate_body(&input, &output) == MICRODUCK_ESTIMATE_UNVERIFIED, "unverified default");
    requireSameRotation(output.bodyToWorldWxyz, {1, 0, 0, 0}, "upright default fixture");
    input.mountVerified = input.jointsVerified = 1;
    require(microduck_estimate_body(&input, &output) == 0 && output.motionAuthorized == 0, "verified still diagnostic");

    for (int fault = 0; fault < 12; ++fault)
    {
        input = makeInput();
        int expected = MICRODUCK_ESTIMATE_INVALID;
        switch (fault)
        {
            case 0: input.abiVersion = 2; break;
            case 1: input.imuSampleNs = input.nowNs + 1; break;
            case 2: input.imuSampleNs = input.nowNs - 50000001; expected = MICRODUCK_ESTIMATE_STALE; break;
            case 3: input.velocitySampleNs = input.nowNs - 40000000; expected = MICRODUCK_ESTIMATE_UNSYNCHRONIZED; break;
            case 4: input.sensorToWorldWxyz[0] = std::numeric_limits<double>::quiet_NaN(); break;
            case 5: input.sensorGyroRadS[0] = std::numeric_limits<double>::infinity(); break;
            case 6: input.jointVelocityRadS[0] = 50; break;
            case 7: input.jointAnglesRad[3] = 0.5; expected = MICRODUCK_ESTIMATE_JOINT_LIMIT; break;
            case 8: input.sensorToSiteWxyz[0] = 0; break;
            case 9: input.reserved = 1; break;
            case 10: input.jointsVerified = 2; break;
            case 11: input.jointSampleNs = 0; break;
        }
        require(microduck_estimate_body(&input, &output) == expected, "reject bad input with precise status");
        require(output.estimateAvailable == 0 && output.motionAuthorized == 0, "invalid data cannot be used");
        require(std::isnan(output.bodyToWorldWxyz[0]) && std::isnan(output.gravityInBody[2]), "invalid values stay unknown");
    }
    require(microduck_estimate_body(nullptr, &output) == MICRODUCK_ESTIMATE_INVALID, "null input");
    require(microduck_estimate_body(&input, nullptr) == MICRODUCK_ESTIMATE_INVALID, "null output");
}

/** 验证编码器时间插值与误差路径，不能使用未来外推或旧回读制造有效速度。 */
void testEncoderAlignment()
{
    MicroduckHeadInput input = makeInput();
    input.imuSampleNs = input.nowNs - 15000000;
    MicroduckEncoderPair pair{};
    pair.previousNs = input.nowNs - 25000000;
    pair.currentNs = input.nowNs - 5000000;
    for (int joint = 0; joint < 4; ++joint)
    {
        pair.previousRaw[joint] = 2046;
        pair.currentRaw[joint] = 2050;
        pair.direction[joint] = -1;
        pair.zeroRaw[joint] = 2048;
        input.jointAnglesRad[joint] = 0;
        input.jointVelocityRadS[joint] = -4 * 6.28318530717958647692 / 4096.0 / 0.02;
    }
    input.jointSampleNs = input.velocitySampleNs = input.imuSampleNs;
    MicroduckBodyEstimate direct;
    MicroduckBodyEstimate interpolated;
    require(microduck_estimate_body(&input, &direct) == 1, "direct reference for encoder interpolation");
    require(microduck_estimate_encoders(&input, &pair, &interpolated) == 1, "aligned encoder samples");
    for (int axis = 0; axis < 3; ++axis)
    {
        require(std::fabs(direct.bodyGyroRadS[axis] - interpolated.bodyGyroRadS[axis]) < 1e-10,
            "encoder velocity and sign agree with direct calculation");
    }
    requireSameRotation(interpolated.bodyToWorldWxyz, {1, 0, 0, 0}, "interpolated midpoint is mechanical zero");
    pair.currentNs = input.imuSampleNs - 1;
    require(microduck_estimate_encoders(&input, &pair, &interpolated) == 4, "no extrapolation");
    pair.currentNs = input.nowNs - 5000000;
    pair.direction[0] = 0;
    require(microduck_estimate_encoders(&input, &pair, &interpolated) == 2, "unknown encoder sign rejected");
    require(interpolated.estimateAvailable == 0 && std::isnan(interpolated.bodyGyroRadS[0]), "unknown speed is not zero");
    pair.direction[0] = -1;
    pair.currentRaw[0] = 4096;
    require(microduck_estimate_encoders(&input, &pair, &interpolated) == 2, "invalid encoder count rejected");
}

/** 执行无硬件测试，输出可保存的 JSON 验证结果。 */
int main()
{
    testMujocoFixtures();
    testDynamicCompensation();
    testValidation();
    testEncoderAlignment();
    std::cout << "{\"passed\":true,\"assertions\":" << assertions
        << ",\"mujoco_poses\":64,\"dynamic_cases\":100,\"invalid_cases\":12,\"hardware_motion\":false}\n";
}
