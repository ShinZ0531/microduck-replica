#pragma once

#include "head_body_estimator.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* 校准 ABI v2 布局保持；未知测量为 NaN，错误文本 UTF-8，角度 deg、角速度 rad/s。 */
    struct MicroduckHeadCalibrationStatus
    {
        unsigned int abiVersion;
        unsigned int nextStep;
        unsigned int sampleCount;
        unsigned int mountAvailable;
        double mountWxyz[4];
        double sampleSpanS;
        double maxBodyErrorDeg;
        double bodyGyroRmsRadS;
        double bodyGyroPeakRadS;
        double headSpanDeg;
        double gyroRelativeErrorRatio; // 新端点契约：三个单向区间中最大的积分旋转相对误差。
        char reason[256];
    };

    /** 创建无 IO 的有界诊断会话；返回空表示分配失败，由 destroy 释放。 */
    void* microduck_head_calibration_create(void);

    /** 释放会话，允许空指针；不得与 observe/capture 并发。 */
    void microduck_head_calibration_destroy(void* session);

    /** 送入同源、递增的新帧；错误输入清空当前采样窗口，不推进校验步骤。 */
    int microduck_head_calibration_observe(void* session,
                                           const struct MicroduckHeadInput* input,
                                           const struct MicroduckEncoderPair* encoders);

    /** 清空当前头部运动证据以开始或重试；仅允许步骤 6–9，不改变既有安装或进度。 */
    int microduck_head_calibration_reset_motion(void* session);

    /** 跳过头部偏航（ID32）自动测试：仅在步骤 8 有效，推进到 9；
     * 不产生测量、不伪造通过，报告以 skipped_yaw_v1 明确标记“未测”。 */
    int microduck_head_calibration_skip_yaw(void* session);

    /** 获取状态，或验证当前步骤（capture=1）；失败保留步骤，可修正姿态后重试。
 * 步骤 0–4：站姿摆正/前倾/后倾/左倾/右倾；前后配对及右倾作为复核；
 * 5：桌面站姿基准；6–9：ID30–33 四停稳端点及三段单向累计旋转；10 表示核验齐全。
 * 成功不写文件、不授权电机；实际运动由原生控制器独占调度。
 */
    int microduck_head_calibration_capture(void* session,
                                           unsigned int step,
                                           unsigned int capture,
                                           struct MicroduckHeadCalibrationStatus* status);

#ifdef __cplusplus
}
#endif
