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

## 第三阶段：专用安装核验闭环（2026-11-13）

新增 `head-calibration.patch`，在前两个补丁之上构成 `.3` 本地控制器（API 44）。
主控真实取样、C++ 求安装及残差、原生报告绑定、Studio 十步向导和上手清单提交闭环已实现。
具体操作、门限和仍需实物验证的限制见[专用说明](head-imu-calibration-20261113.md)。

### 源码及本地验证

- 三个补丁顺序重建的 24 个原生文件与测试构建树逐字节一致。
- C++ 原估计器 1693、新校验器 6554 个断言通过；冻结模型的独立 MuJoCo 夹具保留。
- Rust 共 780 项唯一用例：duck-control 302 常规 + 3 显式 ignored，循环网络 8，
  robotd 331 常规 + 2 ONNX + 3 隔离模式切换，single_instance/updater_gate 11 + 7，robotd-params 113。
- 原提交/helper 78、原 IMU 等 192、原 wizard 12、新头部 8 项 Python 测试通过；
  上述套件有重复组合执行，不把重复执行次数计作新增测试数量。
- 本地统一入口另跑 35 项 Python 和 2 项 JS 检查通过。
- 隔离 Chrome 模拟十步流程包含失败留步、禁止自动保存、单独保存、离页停止轮询及手机宽度检查。
- 按根规范补齐本次修改的方法用途说明，新 C++ 文件通过所附 clang-format 配置检查；
  最后两处格式调整后重建库，所有运行时加载 ELF 节与调整前逐字节相同，仅调试行号节不同。

### 板端部署与只读验收

已通过 SSH 安装到独立本地组件目录，不重新烧卡，不切换托管发布软链接：

| 项目 | 最终值 |
| --- | --- |
| 主控 | `/opt/microduck/local-components/head-body-v3/robotd-head-bmi088.3` |
| 主控 SHA256 | `d2b6b3683d39c9c2a952f6f373676b3a9f2ff64742e7afc9bc84835932f159cf` |
| C++ 库 | `/opt/microduck/local-components/head-body-v3/libmicroduck_head_body.so` |
| 最终库 SHA256 | `06b572547feea279e072cfee6b74209281e5a4af34bad9b450cd8cb0fd17c85c` |
| Studio | `2026.11.13-head-imu.2`，`/opt/microduck/local-components/studio-head-calibration-v3` |
| 最终 Studio 包 SHA256 | `f2ea22b2b312b8e52d4ca7197b99ef07b482138b80166bc7266d399ed049ccec` |
| 原设备备份 SHA256 | `336f18b0e8f107ac60b6efb8aa1006621edc16f323f0e0500048952b75e4cd84` |

先备份全部 `/etc/robot`、原组件、主控与 Studio 服务/覆盖、helper 以及 Studio 状态，
再持有服务管理锁切换。保留 `--commissioning`；设备配置只改变 `trunk_imu.head_estimator_library`。
舵机零位、方向、限位、校准完成标志和安装未核验状态均保留；校准文件摘要前后同为
`62d52690cb9950451db909e5cac033b6a7c60b479086c56b04c143b9fbec0871`。

板端动态库加载正常；不访问硬件的两组 C++ 测试也分别通过 1693/6554 个断言。
切换后的 50 次主控只读采样均保持十五颗反馈、断力、头部来源新鲜：
cycle 增加 251，IMU 序号增加 249，最大年龄 19.60 ms，约 49.99 Hz。
API 回读 44，专用会话 inactive，未保存报告符合当前实物尚未校验的状态。
传感器 tofd 和手柄 padd 运行实例未变。

实际 Studio 使用已授权网页登录完成验收：专用向导可见、源序号持续增加、约 49.6 Hz、
无未捕获网页异常、手机宽度无横向溢出，离开 IMU 页后停止 HAT 轮询。
最后仅注释及格式同步后再次重启；再次检查 370 个 Studio 文件全部匹配摘要。
另采 50 帧只读快照：cycle +259、IMU 序号 +256、最大年龄 20.80 ms、约 50.00 Hz；
主控与 Studio 均 active，自动重启计数为 0。

**本次没有开始真实核验、保存安装结果、提交运动许可、使能电机或执行任何动作。**
需要用户重新加载 IMU 页面，承托机身和头颈，逐步采集并明确保存。
新安装方向的正确性、头部运动补偿精度和后续走路仍待现场验收。

备份在板端 `/var/backups/microduck/20261113-head-imu-calibration-v3/`，
本机第二份及原始日志分别在 `microduck env/device-backups/20261113-head-imu-calibration-v3/`
和 `microduck env/device-validation/20261113-head-imu-calibration-v3/`；均不纳入 Git。
回退入口：

```bash
sudo sh /var/backups/microduck/20261113-head-imu-calibration-v3/rollback-calibration.sh
```

回退会核对本次覆盖与全部设备配置的最终摘要，并要求仍处于断力标定状态。
用户随后保存核验或改配置后，脚本会拒绝自动回退，防止覆盖新成果。
该入口已静态检查，未为演示回退而切换正常服务。

## 第四阶段：通电站姿采集与自动头部测试（2026-11-16）

旧 `.3` 的断力手扶流程难以单人完成，已由用户确认的两阶段流程替代：
先复用原站姿使能，自动采集站立摆正及四向倾斜；再放回桌面继续站立，单独确认自动测试四个头颈关节。
新主控通过同一 Hold 更新目标，正常停止保留站姿，前台续租超时停止轨迹，保存仍需明确确认。
操作、门限与限制以[当前核验说明](head-imu-calibration-20261113.md)为准。

### 源码和本地证据

第四个增量为 `standing-head-calibration.patch`，在前三个补丁上修改五个原文件并新增
`robotd/src/head_imu_standing.rs`。主控本地版本 `.4`、IPC API45、校准 C ABI v2（352 字节）；
估计 ABI v1 和固定躯干验证路径保持原有语义。新版只认可 `head_mount_standing_v2` 证明。

本次重建使用已有离线 Rust 1.99.0、cargo-zigbuild 0.23.4 和 Zig 0.15.2；
robotd 的目标为 `aarch64-unknown-linux-gnu.2.31`。构建树中带补丁 postimage 摘要的 23 个文件
全部匹配最终源码补丁，四个补丁自身 SHA256 校验通过，Studio 包的 370 个文件与仓库逐字节一致。

本次安装前再次执行统一本地入口：C++ 估计器 1693、站姿校验器 47042 个断言通过，
36 项 Python 与 2 项 JS 检查通过；头部专项另跑 9 项通过。
新版模拟浏览器验证自动采齐、头部阶段单独确认、失败留步、停止后不自动恢复、明确保存和离页停止轮询，通过。
完整 Studio 套件在当前主机仍有浏览器/HTTP 用例失败，本次没有据此宣称全库通过。
原生头部专项和动态库用例的本轮构建验证见本机私有 `build-20261116.md`；
此前开发期间的 782 项选定 Rust 回归与实物动作验收是不同证据。

### 安装前修正及板端更新

预检发现先前上传的动态库误用本机 x86-64 构建（`994f6651...dd99f`）。
该文件没有被安装或加载到服务；改用 Zig 为 `aarch64-linux-gnu.2.31` 重建库及两组测试，
更新激活脚本摘要，先在鸭子上确认动态加载和 1693/47042 断言均通过后才切换。

| 最终产物 | SHA256 |
| --- | --- |
| robotd `control.48-head-bmi088.4` | `31275ae07bc8ef1b75cc7c5c9bcb17a81dc375c5d576bd2b951ccb016966fe54` |
| ARM64 C++ 库 | `08eba418da4272a3a54a18c35692f322d6b8ad3541c4f54effe1c8db01953686` |
| Studio `2026.11.13-head-imu.3` 包 | `a90e7f7b6f75b8944df739176bd304325593b1c27896746fcee68c742a8bdc06` |
| 更新前设备备份 | `d6f99f19b915186f88aafef9528977f99393840ac312aca135b2e276cfdc484e` |

激活脚本在维护锁内备份原组件、全部 `/etc/robot`、服务及覆盖、helper 和 Studio 状态。
安装到 `/opt/microduck/local-components/head-body-v4/` 与 `studio-head-standing-v4/`，
只修改设备配置中的 `trunk_imu.head_estimator_library`，并切换主控、Studio 和 helper 路径。
保留 `--commissioning`；舵机零位、方向、限位及 IMU 安装未核验标志没有改变。
校准文件摘要仍为 `62d52690cb9950451db909e5cac033b6a7c60b479086c56b04c143b9fbec0871`。

主控与 Studio 已重启为新版本，两者 active、NRestarts=0，传感器 tofd 与手柄 padd 实例未变。
切换后 50 次只读采样均断力、15 颗舵机反馈有效；cycle 增加 260、IMU 序号增加 258，
最大 IMU 年龄 20.80 ms，约 49.98 Hz。API 回读 45，专用会话 inactive、没有核验报告。

### 实际网页及备份

实际网页登录只读验收通过：原站姿使能入口可用、新通电站姿提示可见，
未站姿使能及确认前采集按钮禁用；HAT 序号推进、约 49.6 Hz、采样年龄 12 ms。
手机宽度无横向溢出，离开 IMU 页停止 HAT 轮询，无未捕获网页异常。
首次隔离浏览器缺少 WebGL，启用该测试浏览器的软件 WebGL 后通过；未为此修改产品程序。

没有开始真实站姿、安装采集、自动头部测试、核验保存或任何走路动作。
实物操作体验、补偿精度及机械间隙仍需用户现场确认，不能由上述软件检查代替。

板端备份路径沿用预定事务名 `/var/backups/microduck/20261113-head-imu-standing-v4/`，
实际执行日期为 2026-11-16。本机第二份位于 `microduck env/device-backups/20261113-head-imu-standing-v4/`，
日志和截图位于 `microduck env/device-validation/20261113-head-imu-standing-v4/`，不纳入 Git。
下载归档 SHA256 为 `f56db62a3990f9716344292df0c8e3fd6d1722eb3913b185f4fe46321e77add0`，
与其中原设备备份分别复核一致。

回退入口为 `sudo sh /var/backups/microduck/20261113-head-imu-standing-v4/rollback-standing.sh`。
它要求部署文件、全部设备配置未被再次改变且仍断力，才恢复 `.3` 主控及 `.2` Studio；
用户随后保存核验或改配置后会拒绝自动回退，防止覆盖新结果。入口已语法检查，未实切回退。

## 停稳端点版（v5）板端部署（2026-11-16）

在 `.4` 已部署的鸭子上切换到停稳端点版：`replica-imu-verify-control.48-head-bmi088.5`、
API 45、证明合同 `head_mount_endpoints_v3`。C++ 组件升到 1.3.0，新增
`head_endpoint_verification.cpp` 与 `microduck_head_calibration_reset_motion`，
四停稳端点与三段单向陀螺积分由独立源文件计算；校准 C ABI 仍为 v2 布局。

| 产物 | SHA256 |
| --- | --- |
| robotd `.5` | `77bf1a2dddf485c589439415f6ce60200cd07bcce556aaa3420c575bf482dec7` |
| ARM64 C++ 库 1.3.0 | `1e6b43f29908413de37e24fe5fe326305094d4bbee5d4d18d594466816075e4a` |
| Studio `2026.11.13-head-imu.4` 包 | `5e0a4f5e67807d1e384b8a303485cf5047674f094521303a5b73c1ececec3027` |
| 部署前设备备份 | `1777b3ada6c4fb16f19132796229cc828b26d979b0aa3052ba4f75b43c76d859` |

本机为 x86-64、鸭子为 aarch64，robotd 与 C++ 库都用 Zig 按
`aarch64-unknown-linux-gnu.2.31` 交叉构建，安装前核对 ELF 与动态依赖。
本地 C++ 估计器 1693、端点校验 103242 个断言通过；robotd 头部站姿/校准用例和
真实动态库用例通过，Python 头部相关 56 项通过。串口伪终端用例受本机 PTY 权限限制未跑通，
没有据此宣称整库通过。

安装到 `head-body-v5/` 与 `studio-head-endpoints-v5/`，只改设备配置中的
`trunk_imu.head_estimator_library`、两处服务覆盖与 helper 路径，保留 `--commissioning`。
切换后 50 次只读采样：cycle 增加 256、IMU 序号增加 254，最大年龄 20.64 ms，约 50.0 Hz；
15 颗舵机反馈有效、断力、安装未核验；API 回读 45，会话合同为 `head_mount_endpoints_v3` 且 inactive。
校准文件摘要仍为 `62d52690…`，tofd 与 padd 实例未变化。

安装脚本曾把 `.5` 可执行文件写成 `robotd-head-bmi088.4` 文件名；随即改名并同步覆盖，
助手期望的 commissioning 覆盖摘要逐字节核对一致，仅重启 robotd，主控仍断力标定。
修正记录与更新后的文件摘要保存在板端备份目录，回退入口保持可用。

没有开始真实站姿采集、自动头部测试、核验保存或任何运动。
回退：`sudo sh /var/backups/microduck/20261116-head-imu-endpoints-v5/rollback-endpoints.sh`。

## 偏航跳过（v6，2026-11-16 已上机）

按用户要求，ID32 偏航自动测试改为**跳过**：不移动关节、不采集样本，也不把跳过当成实测通过。
C++ 校准核心新增 `microduck_head_calibration_skip_yaw`（仅步骤 8 有效），报告写入
`method=skipped_yaw_v1`、`skipped=true`、样本与端点计数为 0；robotd 调度在同一 Hold 内直接推进到 ID33。
助手与 Studio 只接受这一零证据标记，其他步骤仍须完整端点与单向积分证据；换步、带数值或缺少标记均拒绝。

本地验证：C++ 109468 断言（含仅步骤 8 可跳、跳后仍需 ID33 实测）、Rust 头部校准用例、
Python 头部相关 57 项通过；六补丁链从 `eecac18` 重建的 robotd 与工作树逐字节一致，
ARM64 产物摘要固定。已部署为 `control.48-head-bmi088.6`、C++ 1.4.0、Studio `2026.11.13-head-imu.5`；
切换后 50 次只读采样正常（约 50 Hz、断力、15 颗反馈），合同摘要、配置与校准文件不变。
真实站姿采集与偏航跳过行为尚未由用户实测确认。

## 核验报告安装四元数的 1 ULP 比较问题（v7，2026-11-16 已上机）

用户完成 v6 十步核验并保存后，主页上手清单仍把 IMU 判为未完成。板端
`calibration-status` 的 `head_imu_verification` 为 null，而报告文件本身十步齐全
（ID30/31/33 端点姿态差约 2.68–2.73°、积分比 0.06–0.17，ID32 为明确跳过行）。

原因：`verified_proof` 用 JSON 值相等比较报告里的安装四元数与标定文件里的四元数。
校准 TOML 与报告 JSON 由不同的浮点解析器读入，同一个十进制数会差 1 ULP
（实测首分量 `…c53` 与 `…c52`），于是报告被判不匹配，界面显示“未完成”。

修复：比较改为按单位四元数夹角，容差 1e-6（约 0.16°），仍拒绝真正不同的安装
（0.5° 偏差用例被拒绝）；新增回归覆盖 1 ULP 接受、换安装拒绝。
主控版本升为 `control.48-head-bmi088.7`，C++ 库与 Studio 包内容不变（Studio 仅改助手
`HEAD_START` 指向 v7）。本地用板端真实报告复跑 `verified_proof` 已返回通过；
C++ 109468、Rust 头部用例、Python 57 项通过。已部署为 `control.48-head-bmi088.7`、
Studio `2026.11.13-head-imu.6`；切换后 `calibration-status` 返回 10 条报告，
部署后的 `onboard.imu_row` 实测为 `ok`，校准与证明文件未变。第一次执行在写结果 JSON 时
引用了不存在的字段，事务自动回退 v6；清理后修正字段重跑成功。
