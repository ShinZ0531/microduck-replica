# 头部 BMI088 躯干姿态与安装核验

本模块将旧工程中的头部补偿核心和原生接入增量整理为可追溯源码。
输入是头部 BMI088 的源时间、四元数、角速度，以及 ID 30/31/32/33 的实际关节反馈。
输出是诊断用躯干姿态、角速度和重力方向；不会授权运动。

## 本地构建与测试

需要 Linux、CMake 3.16+ 和支持 C++17 的编译器：

```bash
cmake -S software/head-body-imu -B software/head-body-imu/build -DCMAKE_BUILD_TYPE=Release
cmake --build software/head-body-imu/build -j2
ctest --test-dir software/head-body-imu/build --output-on-failure
```

已有 Python 3 与 Node 22+ 时，可从任意目录运行一键本地验证：

```bash
bash software/head-body-imu/scripts/verify-local.sh
```

该入口需要允许创建本机模拟 Unix socket 和 HTTP 连接，不访问真实鸭子。

测试覆盖模型参考姿态、叠加头部运动时的躯干估计、编码器时间对齐及无效输入。
库通过 C ABI 提供接口，布局固定为输入 200、编码器双帧 80、输出 152 字节。
四元数为 Hamilton `w,x,y,z`，角度为 rad、角速度为 rad/s、时间为同一单调时钟的 ns。

目录职责：`source/` 是算法和已有回归测试，`patches/` 是独立运行时补丁，
`scripts/` 处理本地源码准备与验证，`SOURCE.json` 记录基线、模型摘要及诊断限制。
不包含 ARM64 二进制、设备校准、镜像或工具链。

`config/head-source.example.toml` 只列出头部来源需要的三个参数。它不是完整设备配置，
不随构建自动安装，也不包含历史零位、模型选择或安全开关。

## 原生运行时接入

`microduck-replica` 的 Studio 是 Python/JS；`robotd` 来自独立的原生仓库。
本次先固定旧工程已经验证数据通路的 control.48 基线，不把 RC4 或官方新版本直接混入。

在已有的、完整且干净的 [Microduck-kissqy](https://github.com/fanhao375/Microduck-kissqy) checkout 中，
HEAD 必须为 `eecac1878136d3188370a5f6460b7d0521cf845d`。先检查，再选择应用：

```bash
bash software/head-body-imu/scripts/prepare-runtime.sh /path/to/runtime --check
bash software/head-body-imu/scripts/prepare-runtime.sh /path/to/runtime --apply
```

脚本不下载、不编译、不连接鸭子；拒绝基线不符或已跟踪文件未提交的 checkout，
临时索引按顺序验证四个补丁，也会拒绝覆盖新增目标文件。
`head-bmi088.patch` 包括 11 个原有文件的修改与 3 个新增 Rust 文件；
`runtime-hardening.patch` 在此基础上修改 9 个已有文件，另加一个仅用于测试的起身配置。
后一补丁使相同 HOME/P 部署参数可重复初始化，但仍拒绝更换已经采用的参数，
并修正启动阶段显示及过期测试假设，不改变实际设备的零位或按键配置。
`head-calibration.patch` 再修改 6 个原有文件、新增 1 个 Rust 绑定文件，实现本文下述专用流程；
`standing-head-calibration.patch` 在其上增加通电站姿自动采集、头部轨迹和停止机制。
当前本地控制器为 `control.48-head-bmi088.4`、API 45，校准 ABI v2 为 352 字节；估计 ABI v1 保持。
原生仓库的 Apache-2.0 许可保存在 `patches/LICENSE.runtime`。

应用后按原生仓库 `CONTRIBUTING.md` 的工具链构建。该基线要求 Rust 1.99+，
Linux 构建还需要其原有 C 库、模型及 ONNX 运行库。可复跑本轮选定原生目标的完整验证：

```bash
export ORT_DYLIB_PATH=/path/to/host/libonnxruntime.so.1.28.0
bash software/head-body-imu/scripts/verify-native.sh /path/to/runtime /path/to/separate-build
```

除 Rust/Cargo 和 CMake 外，该入口需要 Node 22+、jq、bubblewrap，
以及允许本机模拟 socket 和隔离挂载空间的 Linux 环境。模型必须事先备齐：
`packaging/models` 和 `packaging/roller` 中的 11 份原始 `policy.onnx` 均按各自 manifest 核对摘要。
脚本不自动下载、安装依赖或向主机 `/opt` 写文件；基线和候选必须使用不同构建目录。

2026-11-13 的 `.3` 候选已通过 C++ 原估计器 1693 与新校验器 6554 个断言，
以及选定原生目标的 780 项唯一 Rust 测试。默认 ignored 的动态 ABI、循环网络、
ONNX 推理和三项模式切换测试均显式执行。三项模式切换用例在隔离目录布局中使用
明确的常量网络替身，只验证状态机；其他真实动作模型用例仍使用核验过的原始模型。
Python 回归和模拟网页完整十步流程也通过。这些结果不代表实物安装或走路已通过。
历史部署与实物验收边界见[整合记录](../../docs/head-imu-integration-20261113.md)。

## 与 Studio 的关系

Studio 的 IMU 页新增原始 HAT 卡片；`/api/head-imu` 复用 tofd 只读订阅、正常网页登录和关闭清理。
标定模式的躯干状态来自 `robot.busStatus.last_sample`，显示源数据与网页接收年龄。
原始传感器姿态与经关节补偿的躯干姿态分别标注。

不迁移旧 `/api/body-imu-preview`：它依赖 Studio 自己接管舵机总线，
不作为 robotd 控制时的数据来源。C++ 计算只在原生控制器接入层执行。

## 专用安装核验与下一步

Studio 在 `head_bmi088` 来源下保留原有站姿使能与卸力入口。
第一段通电站姿下自动采集摆正、前倾、后倾、左倾、右倾，操作者双手握躯干提起并倾斜整机。
第二段放回桌面保持站立，经单独确认后，主控自动采集新基准并缓慢测试 ID30–33 约 ±5°。
停止或页面续租超时会停止轨迹、保留站姿，不自动恢复；紧急卸力继续沿用原有控制路径。
原生读取真实 HAT 和编码器反馈，C++ 计算安装和残差；页面不能上传替代测量。
全部通过后仍需单独保存，保留机械零位和方向，策略运动开关仍关闭。
之后扶稳卸力，回上手清单预览并明确提交，由 helper 复核报告、重启并回读运动模式；失败回退。

操作步骤、门限、报告绑定和失效条件见[专用校验说明](../../docs/head-imu-calibration-20261113.md)。
`motionAuthorized` 恒为 0；原生固定躯干 `verify-imu` 继续拒绝头部来源。
后续坐站、蜷缩碰撞和行走都需要独立的实物验收。
