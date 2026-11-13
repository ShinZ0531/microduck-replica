# 头部 BMI088 整合与上机验证记录

日期：2026-11-13，北京时间。分支：`integration/head-imu`。

当前状态：Studio 保持 `2026.11.13-head-imu.1`，原生主控已更新至
`replica-imu-verify-control.48-head-bmi088.2`。本轮选定原生目标的 775 项 Rust 测试通过，
主控切换及只读实机采样通过；头部安装核验和真实动作验收仍未完成。后续章节保留分阶段证据。

## 实现结果

以复刻仓 `1040543544362e2e4962333d8172fa08401665d9` 为基线，选择性移植旧工程的四部分：

- Studio 只读 HAT 读取器与 IMU 卡片，使用源采样时刻判断年龄及频率。
- 标定模式 `last_sample` 的舵机与 IMU 显示，保留断线、过期和未核验状态。
- 独立 C++17 补偿核心、C ABI、模型夹具及 CMake 构建/安装入口。
- 固定 `Microduck-kissqy` 的 `eecac187...`，保存 11 个已有文件与 3 个新增 Rust 文件的完整补丁。

源码入口、坐标/时间约定与构建方法统一见
[头部 IMU 组件说明](../software/head-body-imu/README.md)。工程规范入口见
[engineering/README.md](engineering/README.md)，规则副本与原规范包摘要可在仓库内追溯。

保留当前主线的手机中控、HTTPS、语音和网络实现；没有用旧 Studio 整目录覆盖。
本轮没有混入 RC4 发布线，也没有复制镜像、工具链、旧二进制或本机 EEPROM 参数。
旧 HTTP 躯干预览依赖舵机页接管总线，本轮不迁移该控制旁路。

## 合并中解决的差异

1. 较新读取器修正了“接收时间当作采样时间”的问题，未采用旧同名文件。
2. 无完整 `last_sample`、但已有独立启动观测时，保留主线的舵机/ID 200 诊断；不让空标定映射遮住它。
3. 一个旧 IMU 测试只提供重力值，没有源时间和当前连接证据。补齐模拟数据，生产代码仍严格拒绝无时效证据的读数。
4. 原始头部姿态与躯干姿态分别标注；浏览器显示参考不会写入安装标定。

## 整合阶段的本地验证

| 检查 | 结果 |
|---|---|
| CMake Release 构建、CTest | 通过；原有 C++ 测试含 1693 个断言 |
| CMake 安装到临时目录 | 通过；头文件及 `.so`/SONAME 符号链接齐全，无系统安装 |
| Python 相关回归统一执行 | 334 项通过，约 137 秒 |
| 新增一键验证脚本 | 通过；含 C++、27 项只读网页/订阅测试和 2 项 JS 测试 |
| JS 姿态显示与既有手机指令回归 | 2 + 9 项通过 |
| JS 语法、Bash 语法 | 通过 |
| 原生补丁 | 可应用到 11 个原始前像；应用后 14 个文件与旧定制版本逐字节一致 |
| 准备脚本拒绝路径 | 缺参数、非法操作、错误运行时基线均拒绝 |
| 新增 Python 类型/具名方法说明 | AST 检查均有 docstring，并人工核对用途与实现 |
| Git 差异空白检查、主要文档本地链接 | 通过 |
| 旧工程审计输入 | 55 个文件摘要保持不变 |

统一 Python 验收涉及 `test_head_imu`、`test_commissioning_view`、`test_sensors_http`、
`test_http`、`test_imu_backend`、`test_startup_status`、`test_servo`、`test_commit`、
`test_imu_home_coarse`、`test_imu_diagnostics` 和 `test_imu_calib`。
Python 3.14 下输出包含依赖弃用提示和 socket 资源警告，未将其描述为“无警告”；用例最终均通过。

以上整合阶段测试使用本机模拟 Unix socket、伪终端或 HTTP 服务，没有连接鸭子。
该阶段未安装额外依赖或全局规范，没有执行全库格式化。规则中的方法说明、职责和失败语义仍需人工审阅，
AST/格式检查不能替代语义判断。

## 2026-11-13 SSH 部署与真实验证

用户授权本地及上机测试、SSH 维护后完成本节。部署源码固定为 `8d229c287190bd19d985aa1b0a672a0442a02f09`；
本节后续文档提交不改变该运行包。

### 设备与变更范围

- Armbian 26.2.1 / Debian 13，ARM64，Python 3.13.5。
- 原 Studio 为 `2026.11.4`；主控为 `replica-imu-verify-control.48-head-bmi088.1`，API 43。
- 主控仍在 commissioning，15 颗舵机有反馈、扭矩状态为 `off`；头部 IMU 安装方向未核验。
- 先完整备份 `/etc/robot`、三组服务及覆盖、Studio 状态、助手、旧软件和头部定制组件。
- 板端备份位于 `/var/backups/microduck/20261113-head-imu-8d229c2/`，本机另存副本。
  归档 SHA256：`e5a0bfc6c563194809ca384e85f707a2bc3cb5b9c6cd8c4bef7a3414c9c20bb5`。
  备份含设备私人配置和登录状态，不纳入 Git。
- 新 Studio 369 个白名单文件逐一核验摘要，安装至
  `/opt/microduck/local-components/studio-head-imu-8d229c2/studio`。
- 唯一新增服务覆盖为
  `/etc/systemd/system/microduck-studio.service.d/zzzzzzz-local-head-imu.conf`，只改变 WorkingDirectory。
  `/usr/local/sbin/microduck-helper` 的包装入口同步指向该目录的配套助手。
- `/opt/microduck/software/current` 继续指向原受管理版本；开发验证版不伪装成上游签名发布。
- 仅重启 Studio；事务核对 robotd、tofd、padd 的 PID/InvocationID 均未变化，
  `/etc/robot` 全部文件摘要一致，扭矩仍为 `off`。助手 `calib-read` 成功。

当前 HTTP `/healthz` 报告 `suite_version=2026.11.13-head-imu.1`，服务为 enabled、active，
自动重启计数为 0。该阶段只验证了服务重启；随后用户关机再开，本轮 SSH 检查确认网页覆盖仍然生效。

### 新增实测证据

| 检查 | 结果与边界 |
|---|---|
| 一键本地回归复跑 | C++/CTest、27 Python、2 JS 通过 |
| Rust 主机编译 | 1.99.0 编译出测试二进制；不等于完整测试通过 |
| Rust/C++ 动态 ABI | 2 项通过，显式运行原本 ignored 的真实动态库用例 |
| 原生头部订阅器 | 3 项通过：源时钟、报文限长、模拟 tofd 协议 |
| robotd-params | 113 项通过 |
| ARM64 构建 | robotd、C++ 动态库和算法测试程序均构建成功；Zig 0.15.2 / cargo-zigbuild 0.23.4，glibc 目标下限 2.31 |
| ARM64 实机算法测试 | 1693 断言通过；64 模型姿态、100 动态案例、12 无效案例；无电机访问 |
| ARM64 加载检查 | 候选 robotd `--help` 成功、动态依赖齐全；C++ `.so` 能加载且拒绝空指针 |
| 新 Python 读取器实机订阅 | 149 个有效快照、约 49 Hz、最大源年龄 31 ms、四元数归一化和关闭清理通过 |
| 真实浏览器 | 对实际板端服务验收；实时序号增长、约 49.6 Hz、样本年龄 10 ms、安装未核验标志保留 |
| 页面行为及视觉 | 桌面/390px 手机截图人工查看；SVG 有效，显示参考设置/恢复成功，无横向溢出，离页停止轮询，无未捕获异常 |

主控 ARM64 候选 SHA256：`fe63282f9e04998c4f30e0fc25fa0cead4823c457e71aaff9927340b5ba7ecb5`。
C++ ARM64 库 SHA256：`836f9be5fcd05aeb783ddfd375366ce3cd75b7dabd3c869d3b9a62b4f2172a24`。
候选文件只放在板端 `/home/radxa/microduck-validation-8d229c2/`，没有覆盖运行中的主控或库。

工具链从旧工程归档校验摘要后解压；补充构建工具、依赖和产物均在本机
`/tmp/microduck-local-validation/`，没有系统全局安装。板端约 84°C，因此未在板端执行长时间编译。
临时目录可能随系统清理，需长期保留的报告、截图及事务脚本另存于工作区的设备验证目录。

### 上一阶段未通过项与持续限制

- 原生首次完整回归：duck-control 为 293 通过、5 失败、2 忽略；robotd 为 288 通过、43 失败、2 忽略。
  发现缺失模型文件、主机缺少 libonnxruntime、进程共享初始化及已有快照断言差异。
  已在临时构建树中按 manifest SHA256 找回 6 个同内容模型副本，但没有重跑并完成完整回归与基线对比。
  当时未据此宣称原生完整回归通过，也未将新候选激活为控制服务。
  该回归问题现已处理，结果见下节；摄像头及实物动作限制仍然存在。
- 既有摄像头 sensors_e2e 在 Chrome 的手机视频尺寸断言失败（videoWidth/videoHeight 为 0），未改动摄像头代码。
  头部 IMU 的独立实机网页验收已通过，两者分别报告。
- 首次无头浏览器缺少软件 WebGL，3D 初始化异常阻断 hooks；补上测试所需的
  `--enable-unsafe-swiftshader` 后独立验收通过。没有用修改生产代码掩盖测试环境问题。
- 本次为独立开发目录部署，受信任签名发布入口会拒绝原生自动更新，并可能记录
  `native_untrusted_release`；不修改信任校验来放行。当前开发版继续通过 SSH 管理，
  后续转正式软件包前需撤销本地覆盖并完成完整发布验证。
- 未实现头部专用安装方向与关节方向的实物核验/保存流程，未测试坐站、蜷缩碰撞、起身或行走。

### 回退

本次专用回退脚本已保存在板端备份目录，拒绝覆盖本次部署之后的人工修改：

```bash
sudo sh /var/backups/microduck/20261113-head-imu-8d229c2/rollback-studio.sh
```

它只撤销本次 Studio 服务覆盖、恢复原助手入口、重启网页并确认恢复为 `2026.11.4`。
脚本已语法检查并核对文件前像；没有为了测试回退而再次切换正在正常运行的版本。
主控、tofd、零位和校准文件均不在该脚本的修改范围。

## 原生回归修复与诊断主控第二轮部署

本节接续前述失败调查。旧工程目录保持不变，原生源码和构建工具只在本机临时目录操作；
源码变更以独立 `runtime-hardening.patch` 保存，没有将原生仓库整目录或模型纳入本 fork。

### 失败原因与修复范围

- 缺失模型及主机 ONNX 运行库：在临时目录准备主机版 ONNX Runtime 1.28.0；
  找回全部 11 份原始模型并逐一匹配 manifest SHA256，没有用其他真实策略冒充原模型。
- 相同环境下对比未打头部补丁的 control.48 基线，初始化与测试假设差异也存在于基线。
  原先分开安装 HOME/P 的两个 OnceLock 改成成对原子安装；重复确认同一组参数成功，
  不同 HOME 或 P 仍报错，原值不变。新增独立单例测试覆盖重复、冲突和并发。
- 启动报告继续优先于缓存快照，同时识别旧 `configuring_uart` 阶段。
  模拟启动测试等待真实读取和启动失败计数，不再把首次有效姿态前的重试当作控制 tick。
  原有无效首帧拒绝 HOME、无目标写入、保持断力的断言保留。
- 起身用例改用独立测试 TOML，避免依赖当前发布配置已变化的 LB 绑定；
  该 TOML 没有部署到鸭子。模式确认测试改为要求不重复排队加载当前模式。
- 快照比较允许源年龄随单调时钟增长，其他内容仍严格比较；
  实际 read 数与控制周期分别核对，不再要求启动首帧也属于控制周期。
- 三项模式切换用例需要可用的安装目录模型布局。统一验证入口使用 bubblewrap，
  在子进程中映射临时 `/opt`，以明确的常量网络替身验证状态切换；主机 `/opt` 不变。
  默认测试将三项标为 ignored 并提示验证入口，入口随后显式运行三项，未以跳过掩盖失败。

两个补丁必须依次应用。准备脚本在临时 Git 索引中顺序验证，`--check` 不修改 checkout。
已验证正常检查/应用、未提交修改拒绝、新增目标文件碰撞拒绝；
两个补丁应用后的 21 个文件与最终构建树逐字节一致。

### 本轮验证结果

入口：`software/head-body-imu/scripts/verify-native.sh`。

| 目标 | 实际执行结果 |
|---|---|
| C++ 核心 | 1693 个断言通过 |
| duck-control 单元/动态 ABI | 301 项常规 + 2 项显式 ignored，通过 |
| 循环网络集成 | 8 项显式 ignored，通过 |
| robotd 单元/运行库/模式切换 | 328 项常规 + 2 项 ONNX + 3 项隔离模式切换，通过 |
| single_instance / updater_gate | 11 + 7 项通过 |
| robotd-params | 113 项通过 |
| Rust 总数 | 775 项唯一用例，0 失败；上述目标默认忽略的用例均已执行 |
| ARM64 重建 | 成功，glibc 目标下限 2.31；保留 Zig 链接优化参数弃用警告 |

没有把测试子进程重复打印的同一项计入总数。验证期间曾再次发现旧快照断言因年龄增加 1 ms
偶发失败，补齐全部相关年龄字段的单调性断言后，统一入口完整通过。
模式切换替身证明状态机行为，不证明真实策略动作正确；本次也不宣称整个原生工作区所有 crate 已测试。

### 板端切换与只读验收

- 先确认旧 `.1` 主控在 commissioning、断力、15 颗舵机反馈有效，IMU 安装仍未核验。
- 新备份目录：`/var/backups/microduck/20261113-head-imu-native-v2/`。
  `before-native-update.tar.gz` 保存原主控组件、主控服务与覆盖、全部 `/etc/robot`。
  SHA256：`512ab2981065f31c97e77d03240f170de9c67568c44b876fd4315b4cc720582b`。
- 新二进制：`/opt/microduck/local-components/head-body-v2/robotd-head-bmi088.2`。
  SHA256：`66703cdc4cdc92763a922993341f45bcc8d7c1c841d6da3b2516f8b7e264d9d4`。
- 只替换 `zzzzzz-head-bmi088-diagnostic.conf` 中的二进制路径，保留全部参数及 `--commissioning`。
  C++ 动态库路径、设备配置、零位、安装核验标志均未改变。
- 持有服务管理锁后仅重启 robotd；Studio、tofd、padd 的 PID/InvocationID 前后一致，
  全部 `/etc/robot` 文件摘要一致。
- 连续 50 次只读采样：控制 cycle 增加 258，IMU 序号增加 256，最大 IMU 年龄 20.36 ms，
  最终采集频率约 50.02 Hz；每次均为 15 颗反馈、无当前采集错误、扭矩 off、安装未核验。
- Studio 健康检查仍为 `2026.11.13-head-imu.1`；主控 active，自动重启计数 0。
  新主控验证了服务启动，未对 `.2` 再做整机断电测试，未发出任何使能或动作指令。

双份备份与原始日志位于本机 `microduck env/device-backups/20261113-head-imu-native-v2/`
和 `microduck env/device-validation/20261113-native-hardening/`，包含设备信息的资料不纳入 Git。
本轮原生回退入口：

```bash
sudo sh /var/backups/microduck/20261113-head-imu-native-v2/rollback-native.sh
```

回退前核对覆盖前像、设备配置摘要及断力状态，只恢复 `.1` 主控路径并验收 commissioning。
脚本已语法检查；未为演示回退而切换正常运行的服务。前述 `rollback-studio.sh` 仍只负责网页回退。

下一步是设计头部安装及关节方向的实物核验，再单独安排真实姿态和动作测试。
固定躯干的 `verify-imu` 拒绝逻辑继续保留，不能以移除它代替头部安装验证。
