#include "head_calibration.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
    using Rotation = std::array<double, 4>;
    constexpr double kPi = 3.141592653589793;
    constexpr double kCountRad = 2.0 * kPi / 4096.0;
    const Rotation kMount{0.5, 0.5, 0.5, 0.5};
    unsigned int assertions = 0;

    /** 本测试不依赖发布模型或设备；失败即终止 CTest。 */
    void require(bool condition, const char* message)
    {
        ++assertions;
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(1);
        }
    }

    /** 合成真实传感器姿态的独立 Hamilton 组合。 */
    Rotation multiply(const Rotation& a, const Rotation& b)
    {
        return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
                a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
                a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
                a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
    }

    /** 单轴整机倾斜；前倾绕 Y 正向，左倾绕 X 负向。 */
    Rotation tilted(int axis, double degrees)
    {
        Rotation value{std::cos(degrees * kPi / 360.0), 0.0, 0.0, 0.0};
        value[axis + 1] = std::sin(degrees * kPi / 360.0);
        return value;
    }

    /** 生成计数离散后的真实关节、IMU 及陀螺数据，源时刻位于两帧中间。 */
    void feed(void* handle,
              unsigned long long& timeNs,
              Rotation body,
              int active = -1,
              bool wrongDirection = false,
              bool wrongGyro = false)
    {
        for (int sample = 0; sample < 130; ++sample)
        {
            MicroduckEncoderPair pair{};
            pair.previousNs = timeNs;
            pair.currentNs = timeNs + 20000000;
            MicroduckHeadInput input{};
            input.abiVersion = 1;
            input.nowNs = pair.currentNs;
            input.imuSampleNs = timeNs + 10000000;
            input.sensorToSiteWxyz[0] = 1.0;
            double angles[4];
            double velocity[4];
            for (int joint = 0; joint < 4; ++joint)
            {
                const double initial = joint < 2 ? 0.25 : 0.0;
                const double amplitude = joint == active ? 0.09 : 0.0;
                const double previous = initial + amplitude * std::sin(sample * 0.04);
                const double current = initial + amplitude * std::sin((sample + 1) * 0.04);
                pair.zeroRaw[joint] = 2048;
                pair.direction[joint] = 1;
                pair.previousRaw[joint] = 2048 + static_cast<int>(std::round(previous / kCountRad));
                pair.currentRaw[joint] = 2048 + static_cast<int>(std::round(current / kCountRad));
                angles[joint] = ((pair.previousRaw[joint] + pair.currentRaw[joint]) * 0.5 - 2048) * kCountRad;
                velocity[joint] = (pair.currentRaw[joint] - pair.previousRaw[joint]) * kCountRad / 0.02;
            }
            Rotation sensorToBody{};
            double relative[3];
            require(microduck_head_kinematics(angles, velocity, kMount.data(), sensorToBody.data(), relative) == 0,
                    "synthetic FK");
            const Rotation sensorToWorld = multiply(body, sensorToBody);
            std::copy(sensorToWorld.begin(), sensorToWorld.end(), input.sensorToWorldWxyz);
            const Rotation inverse{sensorToBody[0], -sensorToBody[1], -sensorToBody[2], -sensorToBody[3]};
            const Rotation spin =
                multiply(multiply(inverse, {0.0, relative[0], relative[1], relative[2]}), sensorToBody);
            for (int axis = 0; axis < 3; ++axis)
            {
                input.sensorGyroRadS[axis] = wrongGyro ? 0.0 : spin[axis + 1];
            }
            if (wrongDirection && active >= 0)
            {
                pair.direction[active] = -1;
            }
            require(microduck_head_calibration_observe(handle, &input, &pair) == 0, "valid synthetic observation");
            timeNs += 20000000;
        }
    }

    /** 拟合三个姿态及独立直立基准；用于成功与故障用例共用相同准备条件。 */
    void prepare(void* handle, unsigned long long& timeNs)
    {
        const Rotation poses[] = {{1.0, 0.0, 0.0, 0.0},
                                  tilted(1, 20.0),
                                  tilted(1, -20.0),
                                  tilted(0, -20.0),
                                  tilted(0, 20.0),
                                  {1.0, 0.0, 0.0, 0.0}};
        for (unsigned int step = 0; step < 6; ++step)
        {
            if (step > 0 && step < 5)
            {
                feed(handle, timeNs, {1.0, 0.0, 0.0, 0.0});
            }
            feed(handle, timeNs, poses[step]);
            MicroduckHeadCalibrationStatus status{};
            const int code = microduck_head_calibration_capture(handle, step, 1, &status);
            if (code != 0)
            {
                std::fprintf(stderr, "step %u: %s\n", step, status.reason);
            }
            require(code == 0, "initial fit and upright verification");
            require(status.nextStep == step + 1, "advance exactly one step");
        }
    }

    /** 十步成功仍只返回核验结果；没有任何电机或文件接口。 */
    void completeCalibration()
    {
        void* handle = microduck_head_calibration_create();
        require(handle != nullptr, "create session");
        unsigned long long timeNs = 1000000000;
        prepare(handle, timeNs);
        MicroduckHeadCalibrationStatus status{};
        for (unsigned int step = 6; step < 10; ++step)
        {
            const auto pose = tilted(2, 30.0);
            feed(handle, timeNs, pose, static_cast<int>(step) - 6);
            const int code = microduck_head_calibration_capture(handle, step, 1, &status);
            if (code != 0)
            {
                std::fprintf(stderr,
                             "step %u: %s, error %.3f gyro %.3f\n",
                             step,
                             status.reason,
                             status.maxBodyErrorDeg,
                             status.bodyGyroRmsRadS);
            }
            require(code == 0, "complete physical protocol simulation");
        }
        require(status.nextStep == 10 && status.mountAvailable == 1, "ten steps complete");
        double product = 0.0;
        for (int index = 0; index < 4; ++index)
        {
            product += status.mountWxyz[index] * kMount[index];
        }
        require(std::abs(product) > 0.99999, "recover sensor-to-head mount, not sensor-to-body");
        require(microduck_head_calibration_capture(handle, 10, 1, &status) != 0, "no eleventh fake step");
        microduck_head_calibration_destroy(handle);
    }

    /** 缺少运动、反向关节以及漏减角速度均不得推进核验。 */
    void rejectInvalidEvidence()
    {
        for (int fault = 0; fault < 12; ++fault)
        {
            void* handle = microduck_head_calibration_create();
            unsigned long long timeNs = 1000000000;
            MicroduckHeadCalibrationStatus status{};
            require(microduck_head_calibration_capture(handle, 0, 1, &status) != 0, "no samples rejected");
            require(microduck_head_calibration_capture(handle, 2, 1, &status) != 0, "cannot skip step");
            prepare(handle, timeNs);
            for (int previous = 0; previous < fault / 3; ++previous)
            {
                feed(handle, timeNs, {1.0, 0.0, 0.0, 0.0}, previous);
                require(microduck_head_calibration_capture(handle, 6 + previous, 1, &status) == 0,
                        "previous joint passes before next fault");
            }
            feed(handle, timeNs, {1.0, 0.0, 0.0, 0.0}, fault % 3 == 0 ? -1 : fault / 3, fault % 3 == 1, fault % 3 == 2);
            require(microduck_head_calibration_capture(handle, 6 + fault / 3, 1, &status) != 0,
                    "bad head evidence rejected");
            require(status.nextStep == static_cast<unsigned int>(6 + fault / 3),
                    "failed evidence never advances or verifies");
            microduck_head_calibration_destroy(handle);
        }
    }
    /** 没有回正、前后重复或左右反向不能被自动采集当成正确步骤。 */
    void rejectTiltLabelsAndMissingReturn()
    {
        void* handle = microduck_head_calibration_create();
        unsigned long long timeNs = 1000000000;
        MicroduckHeadCalibrationStatus status{};
        feed(handle, timeNs, {1.0, 0.0, 0.0, 0.0});
        require(microduck_head_calibration_capture(handle, 0, 1, &status) == 0, "upright fit");
        feed(handle, timeNs, tilted(1, 20));
        require(microduck_head_calibration_capture(handle, 1, 1, &status) != 0, "return level required");
        const Rotation good[] = {tilted(1, 20), tilted(1, -20), tilted(0, -20), tilted(0, 20)};
        for (unsigned int step = 1; step < 5; ++step)
        {
            if (step > 1)
            {
                feed(handle, timeNs, {1.0, 0.0, 0.0, 0.0});
                feed(handle, timeNs, good[step - 2]);
                require(microduck_head_calibration_capture(handle, step, 1, &status) != 0, "wrong tilt label rejected");
                require(status.nextStep == step, "wrong tilt never advances");
            }
            feed(handle, timeNs, {1.0, 0.0, 0.0, 0.0});
            feed(handle, timeNs, good[step - 1]);
            require(microduck_head_calibration_capture(handle, step, 1, &status) == 0, "correct tilt after retry");
        }
        microduck_head_calibration_destroy(handle);
    }
} // namespace

/** 执行完整模拟流程及可导致假通过的故障回归。 */
int main()
{
    completeCalibration();
    rejectInvalidEvidence();
    rejectTiltLabelsAndMissingReturn();
    std::printf("%u calibration assertions passed\n", assertions);
}
