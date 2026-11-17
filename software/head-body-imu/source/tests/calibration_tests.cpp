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

    /** 12 秒端点协议的离散实际位置；可模拟每 240 ms 才跳一次的舵机卡顿。 */
    int rawAt(int sample, int joint, int active, bool jerky)
    {
        double seconds = std::max(0, sample) * 0.02;
        if (jerky)
        {
            seconds = std::floor(seconds / 0.24) * 0.24;
        }
        double offset = 0.0;
        if (joint == active)
        {
            if (seconds < 1.0)
            {
                offset = 0.0;
            }
            else if (seconds < 3.0)
            {
                offset = (seconds - 1.0) / 2.0;
            }
            else if (seconds < 4.0)
            {
                offset = 1.0;
            }
            else if (seconds < 8.0)
            {
                offset = 1.0 - (seconds - 4.0) / 2.0;
            }
            else if (seconds < 9.0)
            {
                offset = -1.0;
            }
            else if (seconds < 11.0)
            {
                offset = -1.0 + (seconds - 9.0) / 2.0;
            }
        }
        const double initial = joint < 2 ? 0.25 : 0.0;
        return 2048 + static_cast<int>(std::round((initial + 0.09 * offset) / kCountRad));
    }

    /** 生成计数离散后的真实关节、IMU 及陀螺数据，源时刻位于两帧中间。 */
    void feed(void* handle,
              unsigned long long& timeNs,
              Rotation body,
              int active = -1,
              bool wrongDirection = false,
              bool wrongGyro = false,
              bool jerky = false,
              int fault = 0)
    {
        for (int sample = 0; sample < (active >= 0 ? (fault == 1 ? 530 : 600) : 130); ++sample)
        {
            if (fault == 2 && sample == 250)
            {
                timeNs += 80000000; // 100 ms 的源间隔：不能用跨缺口积分证明转角。
            }
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
                pair.zeroRaw[joint] = 2048;
                pair.direction[joint] = 1;
                pair.previousRaw[joint] = rawAt(sample, joint, active, jerky);
                pair.currentRaw[joint] = rawAt(sample + 1, joint, active, jerky);
                angles[joint] = ((pair.previousRaw[joint] + pair.currentRaw[joint]) * 0.5 - 2048) * kCountRad;
                // 卡顿用例给陀螺一帧相位差：瞬时差分残差大，但完整单向积分仍对应同一段转动。
                velocity[joint] = jerky
                    ? (rawAt(sample, joint, active, true) - rawAt(sample - 1, joint, active, true)) * kCountRad / 0.02
                    : (pair.currentRaw[joint] - pair.previousRaw[joint]) * kCountRad / 0.02;
            }
            Rotation sensorToBody{};
            double relative[3];
            require(microduck_head_kinematics(angles, velocity, kMount.data(), sensorToBody.data(), relative) == 0,
                    "synthetic FK");
            Rotation sensorToWorld = multiply(body, sensorToBody);
            if (fault == 3)
            {
                const double initial[4] = {rawAt(0, 0, -1, false) - 2048.0,
                                           rawAt(0, 1, -1, false) - 2048.0, 0.0, 0.0};
                double anglesAtStart[4];
                for (int joint = 0; joint < 4; ++joint)
                {
                    anglesAtStart[joint] = initial[joint] * kCountRad;
                }
                Rotation frozen{};
                const double still[4] = {};
                double unused[3];
                require(microduck_head_kinematics(anglesAtStart, still, kMount.data(), frozen.data(), unused) == 0,
                        "frozen sensor fixture");
                sensorToWorld = multiply(body, frozen);
            }
            std::copy(sensorToWorld.begin(), sensorToWorld.end(), input.sensorToWorldWxyz);
            const Rotation inverse{sensorToBody[0], -sensorToBody[1], -sensorToBody[2], -sensorToBody[3]};
            const Rotation spin =
                multiply(multiply(inverse, {0.0, relative[0], relative[1], relative[2]}), sensorToBody);
            for (int axis = 0; axis < 3; ++axis)
            {
                input.sensorGyroRadS[axis] = wrongGyro ? 0.0 : spin[axis + 1];
                if (fault == 4)
                {
                    input.sensorGyroRadS[axis] *= -1.0;
                }
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
    void completeCalibration(bool jerky)
    {
        void* handle = microduck_head_calibration_create();
        require(handle != nullptr, "create session");
        unsigned long long timeNs = 1000000000;
        prepare(handle, timeNs);
        MicroduckHeadCalibrationStatus status{};
        for (unsigned int step = 6; step < 10; ++step)
        {
            const auto pose = tilted(2, 30.0);
            require(microduck_head_calibration_reset_motion(handle) == 0, "fresh independent head trace");
            feed(handle, timeNs, pose, static_cast<int>(step) - 6, false, false, jerky);
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
            require(status.headSpanDeg >= 8.0, "reports real travel before diagnostics");
            require(status.maxBodyErrorDeg < 0.1, "settled endpoints ignore intermediate speed mismatch");
            require(status.gyroRelativeErrorRatio < 0.25, "all single-direction integrals agree");
            if (jerky)
            {
                require(status.bodyGyroRmsRadS > 0.15, "stutter regression would fail old instantaneous gate");
            }
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

    /** 偏航只在步骤 8 可跳过：推进到下一步但不产生测量或端点证据。 */
    void skipYawOnlyAtItsOwnStep()
    {
        void* handle = microduck_head_calibration_create();
        require(handle != nullptr, "create session for skip");
        unsigned long long timeNs = 1000000000;
        prepare(handle, timeNs);
        MicroduckHeadCalibrationStatus status{};
        require(microduck_head_calibration_skip_yaw(handle) != 0, "cannot skip before yaw");
        for (unsigned int step = 6; step <= 7; ++step)
        {
            feed(handle, timeNs, tilted(2, 30.0), static_cast<int>(step) - 6);
            require(microduck_head_calibration_capture(handle, step, 1, &status) == 0,
                    "joints before yaw keep their measured evidence");
            require(status.nextStep == step + 1, "advance exactly one joint");
        }
        require(microduck_head_calibration_skip_yaw(handle) == 0, "skip yaw at its own step");
        require(microduck_head_calibration_capture(handle, 0, 0, &status) == 0, "query after skip");
        require(status.nextStep == 9, "skip advances to the roll joint");
        require(std::isnan(status.headSpanDeg), "skipped yaw has no measured travel");
        require(microduck_head_calibration_skip_yaw(handle) != 0, "cannot skip twice");
        require(microduck_head_calibration_reset_motion(handle) == 0, "start the roll trace");
        feed(handle, timeNs, tilted(2, 30.0), 3);
        require(microduck_head_calibration_capture(handle, 9, 1, &status) == 0, "roll still needs real evidence");
        require(status.nextStep == 10, "ten steps complete with yaw skipped");
        microduck_head_calibration_destroy(handle);
    }

    /** 缺少运动、反向关节以及漏减角速度均不得推进核验。 */
    void rejectInvalidEvidence()    {
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
    /** 缺端点、漏采、姿态冻结及陀螺反向不得通过；重试清空当前证据后可继续原进度。 */
    void rejectIncompleteTracesAndRetry()
    {
        for (int fault = 1; fault <= 4; ++fault)
        {
            void* handle = microduck_head_calibration_create();
            unsigned long long timeNs = 1000000000;
            prepare(handle, timeNs);
            MicroduckHeadCalibrationStatus status{};
            require(microduck_head_calibration_capture(handle, 0, 0, &status) == 0, "query current progress");
            require(std::isnan(status.headSpanDeg), "unmeasured travel is unknown, not zero");
            require(microduck_head_calibration_reset_motion(handle) == 0, "start independent trace");
            feed(handle, timeNs, {1.0, 0.0, 0.0, 0.0}, 0, false, false, true, fault);
            require(microduck_head_calibration_capture(handle, 6, 1, &status) != 0, "bad trace rejected");
            require(status.nextStep == 6, "bad trace cannot advance");
            require(microduck_head_calibration_reset_motion(handle) == 0, "retry drops old failed trace");
            require(microduck_head_calibration_capture(handle, 6, 1, &status) != 0, "reset cannot certify empty trace");
            feed(handle, timeNs, {1.0, 0.0, 0.0, 0.0}, 0, false, false, true);
            require(microduck_head_calibration_capture(handle, 6, 1, &status) == 0, "retry passes with complete evidence");
            require(status.nextStep == 7, "retry preserves mounting and advances exactly one joint");
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
    completeCalibration(false);
    completeCalibration(true);
    skipYawOnlyAtItsOwnStep();
    rejectInvalidEvidence();
    rejectTiltLabelsAndMissingReturn();
    rejectIncompleteTracesAndRetry();
    std::printf("%u calibration assertions passed\n", assertions);
}
