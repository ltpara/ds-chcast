# 开发交接、后续计划与协作约定（2026-11-09）

本文件记录本次暂停开发时的真实状态，作为下次继续工作的入口。当前源码已包含多轮 Windows 发射端、观看端和连接服务改动；**这是开发检查点，不是发布验收完成版**。VP9 / 4:4:4 已接入主要路径，仍处于验证和收尾阶段。

## 1. 保存范围与基线

- 仓库：`ltpara/ds-chcast`。
- 检查点分支：`codex/development-checkpoint-2026-11-09`；实际提交号以该分支 Git 历史为准。
- 开发起点：`ebd689ed680e4c09c55e0e43dd1e5779f2a8d3c0`（原 `main`）。本次保存后续累积的相关源码、已有回归、依赖锁文件、第三方声明及模块文档。
- 当前应用媒体/控制协议：**v61**。主机、服务工作进程、观看端及原生协议工具需要同步构建和升级。
- RustDesk 比较固定于本地参考提交 `7423dced37845c48093b6f71bd82e4a16cc8c79d`，不是对其最新发行版的承诺，也不表示线协议互通。
- 按用户要求先保存进度，后续开发等待再次继续；本次不继续扩展功能或执行真实 Flutter VP9 验收。

阅读顺序：本文 → [VP9 / 4:4:4 实现与 v61 协议](vp9-full-color-2026-11-09.md) → 对应模块文档。旧路线图和对比表保留历史快照；它们的“待开发”“已通过”应结合各自日期理解。

## 2. 已实现的功能

下表的“已实现”表示生产路径已经接入。历史模块验收、当前构建和真实设备效果是不同层次的证据，不应互相替代。

| 模块 | 当前实现 | 验证范围及后续边界 |
| --- | --- | --- |
| NAT 穿透、存活与恢复 | 独立签名会合、TCP 预打洞及监听、3/6/9 秒重试、并行直连策略、代理/强制中继、连接结果历史；数据链路保活、自动回退及重新查询鉴权 | 历史 Go / Flutter 检查及本机真实 QUIC UDP 黑洞恢复已验证；跨公网 NAT、IPv6、弱网成功率待实测。见 [连接对齐](rustdesk-alignment-2026-11-08.md)、[NAT](nat-traversal-2026-11-07.md)。 |
| 独立选屏与共享媒体 | 按显示源共享采集编码、每连接独立订阅和有界发送、独立共享系统音源；观看者退出不再决定其他观看者的采集生命周期 | 已有主机会话、订阅与线程归属回归；复杂物理拓扑及跨机拥塞仍待验收。见 [独立选屏](independent-displays-rustdesk-2026-11-08.md)、[多流](independent-streams-rustdesk-2026-11-08.md)。 |
| 全局编码与共享 QoS | 实际解码能力交集、一连接一票、有效 codec 偏好多数票、多人 FPS / 画质、延迟和取帧反馈、积压恢复 | 既有模拟媒体及 QUIC 回归覆盖主要规则；v61 新增色度协商的当前结果另见下文。见 [编码协商](global-codec-rustdesk-2026-11-08.md)、[QoS](shared-video-qos-rustdesk-2026-11-08.md)。 |
| 多屏观看、光标与拓扑 | 原生逐屏解码、Flutter 逐屏画面和光标、热插拔/重编号恢复、故障隔离；子窗口共享认证连接、解码结果与输入归属 | 单物理屏双 Flutter engine / GPU 已有历史验收；多物理屏、混合 DPI、多 GPU、虚拟/隐私拓扑仍需检查。见 [观看端](multidisplay-viewer-rustdesk-2026-11-08.md)、[光标](multidisplay-cursors-rustdesk-2026-11-08.md)、[拓扑](multidisplay-topology-rustdesk-2026-11-08.md)、[共享窗口](shared-display-windows-2026-11-09.md)。 |
| 软件编码与恢复 | vcpkg OpenH264；Auto 创建失败时转软件 H.264，普通硬件编码错误连续三次后协商恢复；固定硬件后端不降软件，设备移除走重建 | 已有真实 WARP / GDI / QUIC 及接口故障注入证据；真实显卡故障和不同驱动仍待验证。见 [OpenH264](software-encoder-2026-11-08.md)。 |
| 摄像头共享 | 同设备共享视频源，与桌面共同协商；附带摄像头复用连接投票，Camera-only 支持参数修改和资源释放 | 模拟媒体与真实 QUIC 已验证，物理摄像头尚未实测；当前摄像头不提供原始 BGRA，完整色彩协商回退 4:2:0。见 [摄像头](camera-shared-video-2026-11-08.md)。 |
| 连接管理与审批 | 稳定连接 ID、指定对象聊天/发文件/权限/断开；服务认证管道和 Click/Both 审批，旧 worker 目标失效 | 历史管理及会话检查已通过；完整跨用户体验待验收。见 [连接管理](connection-management-2026-11-09.md)、[服务管理](service-connection-management-2026-11-09.md)。 |
| 服务设置与管理器 | 机器配置编辑、停止/合并并发更新/按原状态恢复、机器范围 TOTP；SYSTEM 按交互用户启动隐藏管理器、单实例与异常重启监督 | 临时真实 SYSTEM 服务、SCM 与实际入口已有验证；跨用户、RDP 切换仍需验收。见 [机器设置](service-machine-settings-2026-11-09.md)、[管理器启动](service-manager-startup-2026-11-09.md)。 |
| Windows 会话与防休眠 | WTS/RDP 列举和选择、登录代次校验及防 ID 复用；默认开启按已准入连接类型持有/释放电源请求 | 已有真实 SYSTEM/WTS 查询与 Windows 电源请求验证，完整多人切换待验收。见 [会话选择](windows-session-selection-2026-11-09.md)、[防休眠](session-power-2026-11-09.md)。 |
| 主机录屏 | 默认关闭的异步 MP4 视频留档，同源多人只录一份，重建/定时分段、背压与关键帧恢复；当前代码已扩展 VP9 | H.264/HEVC/AV1 属于历史模块范围；VP9 容器专项通过，真实主机和观看端 VP9 录屏链路仍待验收。见 [主机录屏](host-recording-2026-11-09.md)。 |
| 本地 UAC 与接管 | v60 查看端提权请求、本地 UAC、父子身份校验、内存配置接管、保留证书和认证信息、重连及失败恢复 | 真子进程/QUIC 和长暂停已有验证；实际 UAC、另一管理员账户待验收，SYSTEM 便携辅助服务尚未实现。见 [提权](host-elevation-2026-11-09.md)。 |
| 文件与剪贴板 | v56 大目录游标分页；v57 双向/单向/禁止策略及独立文件剪贴板权限；v58 空目录；v59 Windows 虚拟文件和 IStream 按需范围读取、身份/代次/取消约束 | 已有真实 OLE、QUIC、双向文件/目录验收；Explorer 人工操作、跨网吞吐、其他应用虚拟文件来源待验证或开发。见 [分页](file-pagination-2026-11-09.md)、[方向](clipboard-direction-2026-11-09.md)、[目录](clipboard-directories-2026-11-09.md)、[按需读取](clipboard-lazy-2026-11-09.md)。 |
| 键盘与本地 IME | 主/子窗口物理键盘与本地输入法模式、IMM 候选窗、Unicode、提交/取消、文本与快捷键顺序、注入回环过滤 | 历史 Flutter 51 项、原生 3 项及双 Flutter 窗口/微软拼音/真实 QUIC 验证通过；主机为 dry-run，双机目标应用和其他 IME 仍待验收。见 [输入法](local-keyboard-ime-2026-11-09.md)。 |
| VP9 / 完整色彩 | libvpx VP9 profile 0 / 1，原始 BGRA → I444，解码 BGRA 上传 GPU；v61 能力和偏好、全局色度协商、实际格式展示、录屏元数据与分段 | **实现中、尚未整体验收完成**；具体协议、专项成绩和缺口见 [VP9 / 4:4:4](vp9-full-color-2026-11-09.md)。 |

## 3. 本检查点的验证记录

### 已确认完成的构建和专项

- 最后一次原生集成构建退出码 0：`chast-host`、`chast-service`、`chast-protocol-test`、`chast-protocol-fuzz-test`、`chast-desktop-stream-test`、`chast-host-parity-test`、`chast-display-video-test`、`chast-mp4-recorder-test`、`chast-host-settings-test`。日志：`build/vpx-integration-build3.log`。
- Windows Flutter Release 构建和 bundle 安装成功：`build/vpx-flutter-build3.log`、`build/vpx-flutter-install.log`。仍有既存 LNK4098 / LNK4199 及第三方编译警告。
- 真实 libvpx 编解码、重复帧、动态码率/FPS、关键帧和 GPU 上传专项全部通过。320×180 单像素红蓝条纹的平均 RGB 误差分别为 4:2:0 **88.957**、4:4:4 **1.497**；两种格式各 8 帧，GPU 与 CPU 转换像素逐字节一致。
- VP9 MP4 两种 profile 各 8 个样本全部重新解码，检查实际 `vpcC` 的 profile / 位深 / 色度；同 codec 切换色度生成两个文件。日志：`build/vpx-live-acceptance3.log`。小图专项不证明 1080p 实时性能或 Flutter 最终显示效果。
- VP9 阶段的四个既有 Flutter 文件共 **44 项通过**，`flutter analyze` 曾报告无问题；之后还有菜单文字和状态逻辑调整，所以不记为最终全树回归。日志：`build/vpx-flutter-tests.log`、`build/vpx-flutter-analyze.log`。

### 保存前原生回归

本次复跑结果为 **7/8 通过，共 142.18 秒；CTest 退出码 1**。本机证据保存为 `build/checkpoint-native-regression-2026-11-09.log`。

| 测试 | 本次结果 |
| --- | --- |
| protocol | 通过，0.11 秒 |
| protocol_fuzz | 通过，0.72 秒 |
| host_settings | 失败，0.20 秒；3 条断言，原因见下文 |
| host_parity | 通过，111.93 秒 |
| desktop_stream | 通过，4.82 秒；包括修正后的色度投票关键帧检查 |
| display_video | 通过，0.28 秒 |
| viewer_display_streams | 通过，23.57 秒 |
| mp4_recorder | 通过，0.50 秒 |

`host_settings` 有 3 条失败断言：测试输入仍使用 `codec=vp9` 表示未知格式，并在解析函数中断言拒绝 `vp9`。生产代码当前已经支持该格式。下次应将非法格式样本改为真正未知值，并保留未知值回退/警告检查，同时确认 VP9 的合法读取和保存。本次按暂停保存要求如实记录，没有继续修改功能或测试用例。

### 证据边界

- `build/` 的日志和一次性验收程序只保留在本机，不随本提交上传；仓库保存上述摘要、既有测试和可执行的恢复命令。
- 旧模块文档的通过记录属于各自阶段，不代表所有历史场景已在 v61 上重测。
- 未完成真实双 Flutter 窗口 VP9 验收、1080p 性能测量和全树最终回归。
- 当前 GitHub `security` 工作流的 push 触发仅监听 `main`。新分支推送成功不等于已运行或通过 CI；该工作流也不涵盖原生 Windows GPU 全链路。

## 4. 后续开发顺序与完成标准

| 顺序 | 工作 | 完成标准 |
| --- | --- | --- |
| P0-1 | 收尾 v61 原生和 Flutter 回归 | 修正仍把 VP9 当未知值的设置用例，核查本次未通过项；复用现有测试验证合法/非法 codec-chroma、能力交集、同 codec 色度变更、旧帧拒绝和录屏分段。通过后更新本文，保留失败原因和证据边界。 |
| P0-2 | 真实 VP9 / 4:4:4 会话验收 | 用最终库重链临时验收程序；验证真实 DXGI → VP9 → QUIC → GPU → 双 Flutter 窗口，比较 4:2:0 / 4:4:4 截图；第二观看端加入/退出、能力/偏好改变、444→420→444 均能收敛。 |
| P0-3 | 协商与录屏异常路径 | 验证多源 prepare 失败不部分提交，摄像头/Camera-only 导致 420 回退时桌面仍正确；主机和观看端实际录屏在格式切换后可独立播放，状态与文件一致。 |
| P0-4 | 性能、CLI 和打包收尾 | 测量 1080p 编码/解码 CPU、帧率、延迟和码率；核对缩放、旋转、HDR 归一化及逐屏输入。检查所有 codec 分支、诊断与原始流工具；VP9 不得静默按 Annex-B 回放。补齐可复现依赖说明并排除临时验收 EXE。 |
| P1 | Windows 和真实设备验收 | 实际 UAC、跨管理员用户、多用户 RDP、多物理屏、混合 DPI、多 GPU、摄像头、Explorer 粘贴、其他 IME/布局及真实目标应用；记录设备、步骤、预期、结果和限制。 |
| P1 | 网络与硬件恢复实测 | 跨公网 NAT、IPv6、代理、中继、弱网及不同显卡故障恢复；分别记录成功率、恢复耗时和正常观看者受影响情况。 |
| P2 | 后续产品能力 | SYSTEM 便携辅助服务、软件 AV1 / VP8、远程打印、角色互换、其他平台被控仍未实现；单独拆分设计和验收范围，不标作本次已完成功能。 |

## 5. 后续协作与协议变更约定

1. 先读取本文与对应模块文档，再核对分支、工作区和最新测试结果；保留用户修改，避免重复安装已有依赖或从旧失败日志推断新源码状态。
2. 功能优先，按模块完成实现和必要验证。复用已有测试文件/目标；有针对性的真实验收可放本机 `build/`，不为低收益验证新增大量测试工程。
3. 文档分开记录“已实现”“已构建”“自动化通过”“真实链路通过”“待人工验证”。模拟媒体、故障注入和 dry-run 只证明其覆盖的边界。
4. 协议字段、枚举、长度或能力含义发生不兼容变化时，同步更新版本、两端解析/发送、非法输入处理和文档。当前 v61 不承诺兼容 v60，也不与 RustDesk 线协议互通。
5. 以实际编码结果公告 codec、色度和尺寸。共享协商先验证所有源再提交；能力不支持时明确回退或失败，不把 4:2:0 数据标成完整色彩。
6. 依赖通过 vcpkg 管理，记录 triplet、版本和许可证；遇到网络问题按用户指定使用 `socks5://127.0.0.1:10808`，代理仅用于当前命令/进程。
7. 本机状态、PID、身份密钥、证书、运行日志、构建产物和临时验收程序不提交。Windows 打包前检查 Release 目录，避免把辅助 EXE 当产品发布。
8. 后续提交使用 `codex/` 分支。未经后续任务要求，不将这个检查点视为已发布或已完成整个 RustDesk 对齐工作。

## 6. 下次恢复工作

本机已有依赖，不需重复安装：`D:/vcpkg/installed/x64-windows-static-md` 中的 OpenH264 2.6.0#4、libvpx 1.16.0#1、libyuv 1916、libjpeg-turbo 3.1.4.1。详细构建入口见 [VP9 文档](vp9-full-color-2026-11-09.md)。

在已有工作目录先确认没有未保存修改，再切换检查点分支：

```powershell
git status --short
git switch codex/development-checkpoint-2026-11-09
git log -1 --oneline
```

先处理 P0-1；对涉及的已有目标构建后再运行，不使用旧 EXE 的结果冒充当前源码结果：

```powershell
cmake --build build --config Release --target chast-protocol-test chast-protocol-fuzz-test chast-host-settings-test chast-host-parity-test chast-desktop-stream-test chast-display-video-test chast-mp4-recorder-test --parallel 4
ctest --test-dir build -C Release -R '^(protocol|protocol_fuzz|host_settings|host_parity|desktop_stream|display_video|viewer_display_streams|mp4_recorder)$' --output-on-failure
```

然后进入 P0-2。本机已经准备 `build/vpx_flutter_acceptance.cc`、`build/vpx_flutter_main.cc`、编译/链接响应文件和运行脚本；它们仍需用最终库重链。`app/build/windows/x64/runner/Release/vpx_flutter_acceptance.exe` 是临时验收文件，不属于产品。相关脚本不是新机器上可直接依赖的仓库文件。

每完成一个模块，就把结果、剩余项和下一步更新到本文或对应模块文档，保证后续接手不依赖聊天记忆。
