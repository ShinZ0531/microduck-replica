# 头部 BMI088 躯干姿态诊断

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
临时索引按顺序验证两个补丁，也会拒绝覆盖新增目标文件。
`head-bmi088.patch` 包括 11 个原有文件的修改与 3 个新增 Rust 文件；
`runtime-hardening.patch` 在此基础上修改 9 个已有文件，另加一个仅用于测试的起身配置。
后一补丁使相同 HOME/P 部署参数可重复初始化，但仍拒绝更换已经采用的参数，
并修正启动阶段显示及过期测试假设，不改变实际设备的零位或按键配置。
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

2026-11-13 已通过 C++ 1693 个断言，以及 `duck-control`、`robotd`、`robotd-params`
和相关集成目标的 775 项 Rust 测试。默认 ignored 的动态 ABI、循环网络、
ONNX 推理和三项模式切换测试均显式执行。三项模式切换用例在隔离目录布局中使用
明确的常量网络替身，只验证状态机；其他真实动作模型用例仍使用核验过的原始模型。
这些结果不代表真实机器人动作或策略效果已经通过验收。

ARM64 主控诊断版 `replica-imu-verify-control.48-head-bmi088.2` 已通过 SSH 部署并完成只读采样，
仍以 `--commissioning` 启动，舵机保持断力；原 C++ 库和设备配置保持原值。
构建、部署和实测边界见[整合与上机验证记录](../../docs/head-imu-integration-20261113.md)。

## 与 Studio 的关系

Studio 的 IMU 页新增原始 HAT 卡片；`/api/head-imu` 复用 tofd 只读订阅、正常网页登录和关闭清理。
标定模式的躯干状态来自 `robot.busStatus.last_sample`，显示源数据与网页接收年龄。
原始传感器姿态与经关节补偿的躯干姿态分别标注。

不迁移旧 `/api/body-imu-preview`：它依赖 Studio 自己接管舵机总线，
不作为 robotd 控制时的数据来源。C++ 计算只在原生控制器接入层执行。

## 后续验证

- 设备快照、双份备份、Studio 部署与网页实测、选定原生目标回归和主控诊断版切换已完成。
- 固定躯干，验证传感器到头部模型的安装变换和头颈关节方向；再验证身体倾斜时的方向与时效。
- 完成专用校准保存及控制条件检查后，另行进行坐站、碰撞与行走验收。

`motionAuthorized` 恒为 0；保留原生 `verify-imu` 对 `head_bmi088` 的拒绝，
不伪造校准通过，不应用历史 EEPROM 偏移，也不移除板上的诊断模式覆盖。
