#pragma once

/* 固定宽度 POD C ABI；所有数组由调用方持有，调用无分配、无设备访问、无异常。 */
#ifdef __cplusplus
extern "C" {
#endif

enum MicroduckEstimateStatus
{
    MICRODUCK_ESTIMATE_OK = 0,
    MICRODUCK_ESTIMATE_UNVERIFIED = 1,
    MICRODUCK_ESTIMATE_INVALID = 2,
    MICRODUCK_ESTIMATE_STALE = 3,
    MICRODUCK_ESTIMATE_UNSYNCHRONIZED = 4,
    MICRODUCK_ESTIMATE_JOINT_LIMIT = 5
};

/* 所有时间为同一单调时钟的 ns。关节顺序固定为 ID 30、31、32、33。 */
struct MicroduckHeadInput
{
    unsigned int abiVersion;
    unsigned int mountVerified;
    unsigned int jointsVerified;
    unsigned int reserved;
    unsigned long long nowNs;
    unsigned long long imuSampleNs;
    unsigned long long jointSampleNs;
    unsigned long long velocitySampleNs;
    double sensorToWorldWxyz[4];
    double sensorGyroRadS[3];
    double jointAnglesRad[4];
    double jointVelocityRadS[4];
    /* 实测传感器轴 → 模型 head_imu site 轴；未验证时只能用于预览。 */
    double sensorToSiteWxyz[4];
};

/* status=OK/UNVERIFIED 时有计算结果；其余状态所有数值为 NaN，不伪造直立姿态。 */
struct MicroduckBodyEstimate
{
    unsigned int abiVersion;
    unsigned int status;
    unsigned int estimateAvailable;
    /* 本组件仅诊断，此字段始终为 0；成功计算不等于允许运动。 */
    unsigned int motionAuthorized;
    double bodyToWorldWxyz[4];
    double bodyGyroRadS[3];
    double gravityInBody[3];
    double sensorToBodyWxyz[4];
    double headRelativeGyroInBodyRadS[3];
};

/* 相邻两帧同批回读的编码器数据；位置单位 count，方向只能为 -1 或 +1。 */
struct MicroduckEncoderPair
{
    int previousRaw[4];
    int currentRaw[4];
    int direction[4];
    int zeroRaw[4];
    unsigned long long previousNs;
    unsigned long long currentNs;
};

/** 计算头部四关节到 BMI088 site 的旋转与转头角速度。输入单位 rad、rad/s。 */
int microduck_head_kinematics(const double anglesRad[4], const double velocityRadS[4],
    const double sensorToSiteWxyz[4], double sensorToBodyWxyz[4], double relativeGyroRadS[3]);

/** 估计躯干姿态；检查 50 ms 新鲜度和 25 ms 时间偏差。返回 MicroduckEstimateStatus。 */
int microduck_estimate_body(const struct MicroduckHeadInput* input, struct MicroduckBodyEstimate* output);

/** 用包围 IMU 时刻的两帧真实编码器回读插值角度并差分角速度，再调用姿态估计。
 * 间隔只接受 5–40 ms；尚未收到后一帧时返回不同步，不外推，也不伪造零角速度。
 * sensorInput 的 joint* 字段忽略，其余字段和 ABI 语义与 microduck_estimate_body 相同。
 */
int microduck_estimate_encoders(const struct MicroduckHeadInput* sensorInput,
    const struct MicroduckEncoderPair* encoders, struct MicroduckBodyEstimate* output);

/** 对齐两帧编码器，输出供现有估计器及校准器共用的输入；失败输出不得使用。 */
int microduck_align_encoders(const struct MicroduckHeadInput* sensorInput,
    const struct MicroduckEncoderPair* encoders, struct MicroduckHeadInput* aligned);

#ifdef __cplusplus
}
#endif
