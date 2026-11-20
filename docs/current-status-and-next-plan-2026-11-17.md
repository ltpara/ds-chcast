# 当前完成状态与后续计划

日期：2026-11-20（2026-11-18 状态基线延续）

分支：`codex/complete-workspace-2026-11-20`

开发基线：`172516e`；本轮后续实现与状态记录已归档到当前分支。

本文记录截至 2026-11-20 的实际完成状态，作为当前状态入口；阶段明细见 [2026-11-20 W1 状态与后续计划](w1-status-and-next-plan-2026-11-19.md)。详细命令、日志和验收边界仍以各专项记录为准；历史文档中的中间状态不得覆盖本文结论。

2026-11-20 分支核对：通过指定 SOCKS5 代理同步后，确认当前分支完整包含 `main` 与 `codex/s03-role-console-2026-11-17`，本地/远端 HEAD 均为 `172516e`；现有未提交改动保留。后续使用此分支，具体拆单与退出条件见 [分支基线与下一步执行计划](branch-baseline-and-next-actions-2026-11-20.md)。

## 1. 当前结论

项目尚未达到“功能与性能全面对齐 RustDesk”的完成条件。当前为 **W1 外部验收待收口，W2 本机开发继续推进**：审核发现的明确缺陷已修复，S01、S02、S03 的必要代码链路已接通，并取得本地 SQLite、PostgreSQL、浏览器和 Windows 原生进程证据；安装/卸载事务与防火墙 COM 后端已继续加固并通过 fake/隔离回归，但 W1 的双机、正式 SCM 服务、真实 COM 写入回滚和生产环境验收尚未完成。W2 的 C01/C02 已有实现和自动化，C03 正在补齐媒体与多窗口场景；W2–W4 仍有后续实现工作。

当前 schema 为 v26。

## 2. 已完成的修复与实现

| 范围 | 完成情况 | 当前证据与剩余边界 |
| --- | --- | --- |
| F1 地址簿旧缓存覆盖 | 已完成代码修复 | 字段级编辑标记、基准修订和迟到确认保护已纳入回归；真实双客户端离线编辑与升级验收待补 |
| F2 退出后旧同步恢复账号 | 已完成代码修复 | 账号代次、串行写入和旧响应隔离已通过回归；慢网络真实换号待补 |
| F3 过期令牌导致录制不归档 | 已完成代码修复 | 完成录制先入队、上传时取有效凭证、失败保留重试；超过令牌寿命的长录制待实测 |
| F4 管理台换号与刷新竞态 | 已完成代码修复 | 操作绑定账号身份，旧刷新不恢复旧账号；真实浏览器主要流程已补验证 |
| R1–R5 S03 审核缺陷 | 已修复 | 已关闭账号提权、应急权限混用、设备删除权限错配、作用域入口/列表缺失和机器令牌断开回归；SQLite/PostgreSQL 权限矩阵通过 |
| S01 通用 Strategy | 代码闭环完成 | 配置目录、版本/摘要、服务端分配、签名投递、主机与查看端逐键回执、管理台、撤销及重启语义已接通；本地 PostgreSQL、浏览器和实际主机进程已有验证 |
| S02 活动连接与精确断开 | 代码闭环完成 | 快照、指令状态机、幂等重试、回执持久化、代理恢复、旧 runtime 隔离、管理台确认语义及按连接 ID 定向断开已接通；实际托盘、tunnel 和独立 QUIC 文件查看端联动通过 |
| S03 自定义 Admin Role | 代码闭环完成 | 稳定权限键、命名角色、global/group/object 绑定、服务端鉴权、权限上限、最后管理员保护及角色 revision 并发保护已接通；本地 PostgreSQL 24 项无跳过及主要浏览器场景通过 |

Windows 原生进程综合验收于 2026-11-17 首次通过（151.68 秒），并于 2026-11-18 在当前工作区复跑通过（152.82 秒）；随后启用真实浏览器管理台步骤复验通过（161.13 秒）。最新一轮覆盖实际管理台页面精准断开目标 QUIC 会话、目标签名缺席回执确认、非目标会话保持活动、同一 request key 幂等重试，以及受管上限 3/本地上限 2 的准入边界。另有单机原生桌面流验收通过：DXGI 1920×1080、MFX HEVC 硬编、D3D11VA 解码收到 39 帧，文件操作和双终端通过。Flutter `analyze` 通过、814 项测试通过，Windows Release 构建成功；主机策略定向 CTest 重建后 5/5 通过（49.61 秒）。`chast-service` Release 目标也已构建且 `--help` 正常，但没有安装或启动 SCM 服务。最新综合验收目录为 `build/native-runtime-20261118-051723-6d6be834/`，浏览器联测运行目录为 `run-1261003824/`；更早 152.82 秒复跑目录为 `build/native-runtime-20261118-044608-f9561864/`。详细范围与边界见 [原生进程验收](native-runtime-acceptance-2026-11-17.md)。本轮另以隔离 `APPDATA` 启动 Windows Release 查看端，实测 800×600 首页侧栏滚动及服务设置对话框滚动/Tab；将窄窗口回归固定为 `home_page_test.dart` 800×600，首页测试 60/60 通过，`flutter analyze` 无问题，Release 重建成功。

2026-11-18 Flutter 回环补测：真实 Release `SessionPage` 在隔离配置下连接仅绑定 `127.0.0.1` 的主机，完成密码认证和首次主机指纹核对/确认后持续约 266 秒；分辨率 1920×1080、目标 30 FPS，主机发送 7869 帧、查看端收到并解码 7868 帧，发送错误为 0。主机启用 `--input-dry-run`，未发送键鼠或文件操作，输入计数为 0。该证据覆盖单机正常 Flutter 视频会话，不代表键鼠/文件或双机验收。日志位于 `%TEMP%\chast-w1-cert-20261118-f32caa63185c4ac785d419455ce08090\host-final.stdout.log`。单机双查看端并发及非目标端继续收流已有补测；Flutter 视频会话中的管理台精准断开已于本轮通过，双机、SCM 服务及真实网络故障仍待验收。

2026-11-18 Flutter 双查看端单机补测：在隔离 `APPDATA` 下同时启动两个真实 Release 查看端，均通过密码认证及首次指纹确认并显示同一主机桌面流。两端同时在线采样 10 秒，主机/A/B CPU 增量分别约 3.27/2.25/1.70 秒。结束 B 后，主机审计记录其已认证会话正常结束（`success=true`，约 294.8 秒）；A 仍运行，置前后的 2 秒画面样本中 12,040 个采样像素有 11,034 个变化（91.64%）。随后本地结束 A，但审计未出现 A 的成功会话结束记录；因此这项并发补测只证明单机并发收流和 B 结束后 A 仍持续出图，不证明 A 正常关闭，也不单独作为管理台断开证据。管理台精准断开已由下方独立的真实页面/API/主机联测通过。测试未执行键鼠/文件操作，也不是双物理机验收。证据目录为 `%TEMP%\chast-w1-multiviewer-confirmed-20261118-91e2ee0796264b2a81f30f6937e3f083`，主机审计为其中的 `host.audit.jsonl`。

2026-11-18 原生键盘回环探针复验：`chast-keyboard-test.exe` 输出 `keyboard failures=0`；`chast-viewer-probe.exe --keyboard` 退出码为 0。探针通过真实 loopback QUIC、`ViewerEngine` 与主机输入注入，将按键送入本进程拥有的前台编辑框，并核对 US `Chast1`、German `zy/@`（含 AltGr）、Legacy `VK_Y`、Unicode 中文提交及 Microsoft Pinyin `你好`。这验证了 Windows 原生远端键盘注入链路；不等同于 Flutter 窗口、双机输入或文件操作验收。

2026-11-18 当前工作区门禁复跑：Flutter `flutter test` **814/814 通过**、`flutter analyze` 无问题；Go `go test ./...` 与 `go vet ./...` 通过；管理台 `app_session_test.cjs` **47/47 通过**；Windows 原生进程验收启用浏览器步骤后 **161.13 秒通过**。该浏览器步骤是真实管理台页面/API/主机/QUIC 文件模式会话的单机联合验收；不替代 Flutter 视频会话交互、双机及生产环境验收。

2026-11-18 管理台真实断开补测：隔离无头 Chrome 登录内嵌管理台，在活动连接列表打开并核对目标 connection ID，页面提交断开后显示“已确认断开”。真实主机接受命令并签署目标已不存在的回执；目标连接消失、非目标连接保持活动；重用页面实际提交的 request key 重试得到同一 command ID。测试使用两个独立 QUIC 文件模式查看进程，故证明真实管理台页面到 API、主机执行及幂等确认链路，不证明 Flutter 视频会话或跨机网络行为。日志首段记录目标 `35355a54a17bcd169a80881df0f0f856` 与非目标 `98016280833d8bd84916f1915993fa86`；产物在 `build/native-runtime-20261118-051723-6d6be834/run-1261003824/`。

上述“代码闭环完成”不等于功能已完成。只有代码、自动化、实机或生产证据全部满足发布门槛后，才能改为“已完成”。

2026-11-18 Flutter 视频会话管理台断开验收：将窗口自动化从不确定前台的 `SendKeys` 改为直接向目标 Flutter HWND 发送输入，并保存输入前/后的窗口截图。隔离 Release Flutter 查看端通过真实主机认证、首次指纹固定并收到 1920×1080 HEVC GPU 帧后，管理台页面按 connection ID 请求断开；页面显示“已确认断开”，主机签名 `confirmed_absent` 回执被接受，Flutter 窗口进程仍存活，另一个 QUIC 文件模式会话保持 active。随后同一用例完成控制面故障、回执恢复、代理重启、主机崩溃/runtime 隔离及受管 3/4、本地 2/3 会话上限。总耗时 161.32 秒；日志在 `build/native-runtime-20261118-180527-cd863944/acceptance.log`，运行证据在 `build/native-runtime-20261118-180527-cd863944/run-2795422946/`。非目标仍活动的会话为文件模式，未证明其持续视频收流；本轮也不替代真实键鼠/文件交互或双机验收。

2026-11-18 双 Flutter 视频会话管理台断开补测：隔离启动两个独立 Release 查看端，均接收真实 1920×1080/30 FPS HEVC 桌面流并显示统计面板。管理台页面对目标连接执行断开并收到主机签名 `confirmed_absent` 回执；非目标连接仍为 active，查看端进程保持运行。断开页面操作期间，以 250 ms 间隔对非目标 Flutter 会话窗口采样 56 张 PNG，统计面板区域有 50/55 组相邻图像变化；首末截图中接收/解码帧计数从 173 增至 707。该测试没有启用远程键鼠或执行文件操作，也仅是同机 loopback。Go 用例耗时 43.08 秒；日志在 `build/native-runtime-20261118-185528-4b34fb51/acceptance.log`，运行目录为 `build/native-runtime-20261118-185528-4b34fb51/run-1266503374/`。

2026-11-18 本轮补充修复：首页令牌输入框按回车时，使用地址框内容发起连接，避免将令牌作为地址。新增页面回归同时核对目标主机、端口与令牌；测试设备存储覆盖 `touch`，避免写入真实用户配置。修复后首页测试 61/61、Flutter 全量测试 815/815 通过，`flutter analyze` 无问题；管理台 Node 回归 47/47、脚本语法检查及 `git diff --check` 通过。全量 Flutter 日志为 `build/w1-flutter-full-20261118-final.log`。

2026-11-18 最新 W1 本机组合验收：修复管理台连接详情快速切换竞态，旧详情的异步 `close` 事件不再清空已打开的新连接；管理台页面回归 **48/48** 通过。原生验收配置此前把不接受的 `tunnel_listen=127.0.0.1:0` 写入主机，解析器会退回默认通配监听；测试现改为分配独立回环 UDP 端口，实测 tunnel 监听在 `127.0.0.1`。Release Flutter 文件浏览器实际上传、下载 **524325 字节**，源文件、主机接收副本和保留下载副本 SHA-256 均为 `7F942F752AD9EEE5ED89B3EE510E50B5A7A0684B33D07943C315A55F0CE36446`。随后以真实管理台页面断开目标 Flutter 会话；双 Flutter 组合中的非目标连接保持 active，55 张窗口样本里统计区有 46/54 组相邻样本变化。单 Flutter 全流程（含故障恢复、runtime 隔离及连接上限）177.05 秒通过；双 Flutter、文件传输与管理台断开组合 54.66 秒通过。当前工作区 Go `go test ./...`、`go vet ./...` 通过，管理台 Node 回归 48/48，`host_settings` CTest 1/1 通过。证据目录：`build/native-runtime-20261118-214914-882e8a81/` 与 `build/native-runtime-20261118-215415-dbfc4d4a/run-369350478/`。这些都是单机 loopback 验收，不是双物理机、键鼠或跨网验收。

2026-11-18 Flutter viewer 策略真实投递、更新及撤回补测：`tools/test-native-runtime.ps1 -Flutter -ViewerStrategy` 通过（Go 用例 50.90 秒）。首个真实 Flutter 会话的五项 viewer 设置在 revision 1 全部收到 `applied`；保持该会话 active 并更新策略后，第二个会话实际取得 revision 2、五项均 `applied`，首会话原 delivery context 的回执仍保持 revision 1。删除策略分配后，后续第三个会话取得 revision 3 空配置并回报空结果 `applied`。三阶段证据位于 `build/native-runtime-20261118-222855-69bd0658/`。这是单机 loopback 实际 delivery/receipt 及下一会话更新、撤回证据；双机/跨网及 Flutter 键鼠的跨机实机验收仍待完成。

## 2.1 2026-11-19 最新结论

`tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -InputInjection` 已完成单机真实 Flutter 键鼠释放验收：控制开关状态、鼠标与 A-down 下行、主机收到 `InputRelease`、release barrier 入队和 `release_all()` 注入 A-up 均有轨迹证据；A-up 位于物理清理边界之前，边界之后没有第二条远端 A 事件。Go 验收通过，耗时 12.391 秒。范围仅为 loopback + dry-run，不代表双机或真实桌面输入注入。

证据目录：`build/native-runtime-20261119-180114-107055db/run-2803120376/`，主机轨迹为 `input-events.jsonl`。修复前失败目录保留于 `build/native-runtime-20261119-001020-ef1059e1/run-2160080674/`。完整状态与验收边界见 [2026-11-19 状态文档](w1-status-and-next-plan-2026-11-19.md)。

临时 SYSTEM 服务监督验收已有全 PASS 日志，但仅证明临时服务监督路径，不证明正式 SCM `--install`、`--run`、`--worker`、服务身份或安全桌面。

2026-11-19 SCM 只读预检：Release `chast-service` 重建、`--help`、SCM 外 `--run` 拒绝启动，以及 `service_log`、`host_parity`、`host_security`、`host_settings`、`host_multi`、`host_i18n`、`lock_after_session`、`host_ticket_lease` CTest 共 8/8 通过。检查时本机没有 chast 服务注册或 `%ProgramData%\chast\host.ini`。正式安装会写机器配置、创建自动启动服务并调整防火墙，尚未确认本机是可回滚测试环境，因此未执行；范围与后续门槛见 [W1 状态文档](w1-status-and-next-plan-2026-11-19.md)。

## 2.2 2026-11-20 最新结论

SCM 安装/卸载编排新增快照、逆序回滚及 fake SCM/防火墙事务覆盖；停止请求失败时也会查询当前 SCM 状态，并尽力恢复安装/卸载前的运行或暂停状态。防火墙规则创建改用 Firewall COM `INetFwRule`/`Rules.Add`，owner 信任判定有独立纯策略测试。Release `chast-host`、`chast-service` 与 P1 测试目标构建通过；最新完整 P1 CTest 为 18 项中 17 项 PASS、`machine_state_security_untrusted_owner` 因无法启用 `SeRestorePrivilege`（错误 1300）单独 Skipped，总耗时 188.52 秒，`host_parity` 用时 124.10 秒。该 Skipped 不是通过，6 项合成 SID 策略断言也不等同于真实 NTFS owner 集成验收。

本轮未执行正式 `--install`/`--uninstall`、真实 SCM 生命周期、Firewall COM 写入、真实 ProgramData ACL 修改或双机测试；测试前后确认无产品服务/进程、机器配置及产品防火墙规则。真实 SCM/COM 失败回滚、owner 集成分支、生产 PostgreSQL 拓扑和双机/跨网验收仍待专用环境。具体证据与限制见 [2026-11-20 W1 状态文档](w1-status-and-next-plan-2026-11-19.md)。

补充验证：Firewall COM 规则属性现先设协议再设端口；真实 `INetFwRule` 内存对象的 UDP/TCP 配置与读回测试通过。SCM 停止超时的回滚路径现会按查询到的状态等待/恢复原状态，并避免对仍运行的服务重复启动。`chast-host`、`chast-service` Release 重建通过；完整 P1 CTest 复跑为 17 项 PASS、owner 专项因错误 1300 Skipped，总耗时 188.52 秒。测试未调用 `INetFwRules::Add`，不代表真实防火墙规则写入/回滚已验收；只读复查确认无 chast 服务/产品进程、无 `%ProgramData%\chast\host.ini`、无四条产品防火墙规则。

2026-11-20 W2 C01/C02 本机门禁复验：C01 `reference_counted_lease` 与 `viewer_awake_request` Release CTest 重建后 2/2 PASS；Flutter `analyze --no-pub` 无问题、全量测试 825/825 PASS、Windows Release 构建成功。C02 当前工作区包含按设备持久化的视图/光标倍率控制，并覆盖主视图、桌面多显示器布局及独立显示器窗口；新增测试验证变焦输入映射、独立光标热点、普通滚轮远端转发与 Shift+滚轮本地横向平移，并在固定逻辑窗口尺寸下模拟 DPR 从 1 到 2，确认相对输入倍率同步更新。上述均为代码/单机自动化证据，真实混合 DPI、旋转、全屏与多屏视觉验收尚未完成。未启动远端会话、未更改持久电源设置、SCM、ProgramData 或防火墙；双机验证继续后置。

2026-11-20 W2 C03 首个实现切片：独立远端显示器窗口首次打开时，会将远端虚拟桌面布局等比例映射到本机虚拟桌面；单屏远端使用本机主显示器区域。已有按设备/远端显示索引保存的手动窗口位置继续优先。几何边界的 Release CTest 1/1 通过，Flutter analyze、全量测试 825/825 和 Windows Release 构建通过。该切片没有实现多连接标签页、音频/录制状态隔离，也未做真实混合 DPI/显示器热插拔验收；双机验证仍后置。

2026-11-20 W2 C03 本机显示拓扑恢复与隔离回归：独立显示窗口收到 `WM_DISPLAYCHANGE` 后检查当前矩形；可用交集不足 320×240 时，按远端屏幕拓扑重新映射，并在系统显示拓扑尚未稳定时每 250 ms 重试、最多约 10 秒。关闭时及下次恢复时均忽略可用交集不足 320×240 的旧位置。几何/当前显示器及位置持久化 Release CTest 均通过，`display_layout` 连续 10 次通过；位置测试验证 peer/display 索引隔离与更新保留，不模拟显示拓扑变化。没有实际拔插显示器，因此不代表物理热插拔验收。架构审计确认每个独立会话窗口创建自己的 `SharedViewerSession`/`ViewerEngine`；新增 `viewer_session_isolation` CTest 1/1 通过：双 ViewerEngine loopback 的会话音频选项状态、本地录制目录与停止行为彼此隔离，停止一个 viewer 后另一个仍收流/录制，两份 VP9 MP4 均通过重开校验。同进程双 Flutter 窗口 loopback 验收也通过，精确断开其中一条连接后另一条仍 active。真实逐窗口录制/音频 UI、实际 DSP、物理热插拔或双机尚未验证；多连接标签页仍未实现。

2026-11-20 录制文件名并发冲突修复：发现查看端默认文件名只有秒级时间戳，且 FFmpeg 写入会截断同名文件；两个同秒启动的会话可能覆盖录制。`Mp4Recorder` 现只在首个关键帧到达时用 Windows CRT `O_CREAT|O_EXCL` 原子占用候选文件名，冲突时递增编号；首个关键帧前不创建文件，初始化失败则删除本次占位文件。新增并发录制器/分段回归，验证四个 MP4 均可重开、路径互异、既存哨兵内容不变、无关键帧不留下空文件。Release `chast-mp4-recorder-test`、`chast-viewer-probe`、`chast-quic` 构建通过；MP4 专项测试及 `viewer_session_isolation` loopback 探针通过。测试产物仅在隔离目录 `build/test-temp/mp4-exclusive-run-b95d76d7b21c4b9a9d0af6c7b9a394f1/`；保留既有 LNK4098/LNK4199 链接警告。此为单机文件协调和回环证据，不替代双机验收。

2026-11-20 C03 无弱网 WASAPI 播放基线：重建当前源码的 `chast-quic` 后运行 `build\Release\chast-quic.exe verify --audio --play --seconds 25`。QUIC loopback 通过，Opus 48 kHz 立体声收发 2499 包且无缺口/解码错误，解码 24.99 秒；实际 WASAPI 播放 1176960 frames，2 次 underrun、400 ms starved、42 dropped，audio-latency 30.0 ms、A/V skew +22.8 ms。该单次稳定链路 skew 满足既有 ±40 ms 目标；需重复测试评估波动。

2026-11-20 C03 弱网 WASAPI 播放验收：同一重建版本运行 `build\Release\chast-quic.exe verify --netem --audio --play --seconds 25`，使用默认弱网配置（80 ms 基础延迟、20 ms 抖动、3% 丢包、1000 kbps，4–12 秒劣化窗口）。弱网整体验收通过，音频发送 2500 包、接收 2494 包（0.24% 丢包），解码 24.94 秒，无 CRC/解码/协议错误；实际 WASAPI 播放 804960 frames，2 次 underrun、8170 ms starved、812 dropped，audio-latency 26.1 ms、A/V skew +18.0 ms。与单次无弱网样本相比，弱网多出约 7770 ms 空转和 770 个丢弃帧；验收断言并未限制这两项播放连续性指标，因此不能据整体验收通过判定听感质量合格。

2026-11-20 C03 真实麦克风 loopback：构建 `chast-viewer-probe` 后运行 `build\Release\chast-viewer-probe.exe --real-microphone`。ViewerEngine 打开默认 `麦克风 (USB Audio Device)`，实际 WASAPI 采集并通过仅绑定 loopback 的 QUIC 发送；174 个上行包中主机解码 173 个，收到 83160 个采样，RMS 0.4、峰值 27；AEC/降噪处理计数分别为 39840/41280 帧。撤销会话麦克风权限后采集停止，视频又收到 15 帧；全程没有保存录音或播放麦克风数据。该结果证明设备打开、采集、编码、传输、解码和撤权路径；输入电平很低且没有下行播放参考，不能证明可懂语音或 AEC/降噪效果。

## 3. 仍待完成

### 3.1 W1 外部验收

1. 使用第二台 Windows 实机完成跨机 Flutter 查看端、键鼠操作和复杂网络验收。真实 Release Flutter 文件浏览器已在单机 loopback 下完成上传/下载并逐字节核对 SHA-256；键鼠释放序列现已在本机 loopback + dry-run 下通过，跨机文件传输、双机键鼠和复杂网络仍未验收。双 Flutter 组合中，管理台断开目标会话时非目标连接仍 active，55 张样本的统计区域有 46/54 组相邻变化。
2. 自动化覆盖已验证 host 的 default/locked/本地恢复/未知键准入，以及 viewer 的 default/locked/显式本地偏好/未知键回执和会话快照。`SessionPage` 集成回归覆盖五项 viewer 设置：默认建议只在新会话应用、续票收到新建议不改当前快照、下一会话读取新策略，以及本地显式路由/WebSocket/录制/剪贴板/视图偏好优先。真实 Release Flutter 单机已验证五项逐键回执、策略更新由下一会话取得新 revision、分配撤销后下一会话取得空策略；双机/跨网复验仍待完成。受管三连接准入上限边界已完成单机原生进程验收。
3. 在 Windows SCM 服务/worker 路径验证启停、崩溃恢复、账号切换、锁屏和安全桌面；便携托盘运行不能替代服务安装验收。
4. 验证真实断网、重连、跨 NAT、IPv6、代理/CDN、旧回执重放和较长故障窗口；当前 503 只覆盖本机回环控制面。
5. 完成全管理台响应式、键盘和辅助技术覆盖，以及生产 PostgreSQL 拓扑、整库灾备、目标 RPO/RTO 和故障切换验收。本轮修复 tab 键盘导航/ARIA 关联、表格滚动区键盘访问及窄屏双列溢出；Node DOM 契约回归 47 项通过。Chromium 实际检查了 320/375/768/1280px、12 个面板和 18 个表格滚动区，未发现页面/双列溢出或无名称交互控件，并实测 tab 与表格键盘导航；屏幕阅读器和生产数据库验收仍待完成。

### 3.2 W2 日常使用连续性

| 任务 | 状态 | 后续实现 |
| --- | --- | --- |
| C01 | 部分完成 | 连接前默认仅查看偏好、主机 LAN 发现设置及查看端会话期间的 Windows 防休眠请求均已实现；防休眠请求由进程级引用计数共享。C++ 租约、Flutter 会话启动/关闭回归及 Windows Release 构建通过；真实电源行为、多会话实机释放和双机验收仍待完成 |
| C02 | 已实现，单机自动化通过；场景验收待完成 | 按设备保存 5–1000% 视图倍率和 25–400% 光标倍率，支持自适应/原始大小基准；主会话、桌面多显示器布局及独立显示器窗口已接入。自动化覆盖源画面输入映射、光标热点/独立倍率、普通滚轮远端转发与 Shift+滚轮本地平移，并覆盖逻辑窗口尺寸固定时 DPR 从 1 到 2 的相对输入倍率更新。真实混合 DPI、旋转、全屏及多屏人工验收仍待完成 |
| C03 | 部分实现 | 远端独立显示器窗口首次打开按本机虚拟桌面几何自动映射；本机拓扑变化时窗口可用区域不足 320×240 会触发重映射，保存位置优先且离屏位置会被忽略。`display_layout` 覆盖 peer/display 位置持久化与键隔离（连续 10 次通过）；双 ViewerEngine loopback 与同进程双 Flutter 窗口连接隔离回归通过，后者验证精确断开一条连接不影响另一条；录制文件名采用原子独占占位，避免同秒会话和分段互相覆盖。显示器子窗口现提供静音、麦克风上/下行、语音、录制及输出/输入设备和 AEC/降噪菜单，双窗口 widget 回归验证共享状态同步、权限拒绝和菜单操作；合成音频/麦克风 QUIC loopback、干净链路/弱网 WASAPI 输出及默认 USB 麦克风采集链路均通过。干净链路单次播放 400 ms starved/42 dropped、skew +22.8 ms；弱网单次 8170 ms/812 dropped，须重复并设定连续性门槛。真实语音质量、AEC/降噪效果、物理显示器热插拔、混合 DPI/全屏场景和多连接标签页仍待完成 |
| C04 | 待实现 | 剪贴板按键发送、触控板速度/边缘滚动、SVG 剪贴板、可配置快捷键及输入法验证 |
| H03 延伸 | 待实现 | 查看端录制待上传任务跨进程/应用重启持久化恢复；F3 只修复了单次会话内的入队和重试 |

### 3.3 W3 企业协作与治理

| 任务 | 待实现范围 |
| --- | --- |
| A01–A03 | 地址簿密码端到端加密设计，个人与共享簿密钥封装、恢复、轮换、撤权；独立命名共享地址簿；跨设备密文同步，服务端不得保存地址簿明文密码 |
| S04 | Control Role 命名角色及继承/启用/禁用三态覆盖，明确与 Strategy、访问票据和权限上限的求值顺序 |
| S05 | 独立 Alarm 事件模型、查询、管理台、去重、留存和通知接口 |
| S06 | 用户邀请、本地邮箱验证状态、告警邮件偏好和登录设备白名单 |

### 3.4 W4 主机与生产部署

| 任务 | 待实现范围 |
| --- | --- |
| H01 | 可复用便携特权辅助服务、受控本地 IPC、身份绑定、异常清理和升级/退出 |
| H02 | 扩展虚拟显示后端、自定义分辨率、驱动签名/分发条件及多 GPU、HDR、热插拔回退 |
| H03 | 主机入站录制集中上传、断点续传、幂等归档、配额与保留策略联动 |
| O01 | 多实例共享限流、录制存储协调或租约、活动连接路由及指令投递 |
| O02 | PostgreSQL 生产配置、TLS、最小权限、备份恢复、控制面故障切换、滚动升级和监控 |
| O03 | 面向受限网络的自有协议 API 回退通道；需求若是 RustDesk 官方代理协议，则另立条件项目 |
| O04 | 定制客户端配置/品牌管理、构建产物关联、签名、下载、升级与回滚 |

### 3.5 W5 发布候选验收

完成真实 Windows 功能矩阵、双机及复杂网络、RustDesk 同条件 A/B、PostgreSQL 多实例容量和故障接管，以及完整 Flutter、Go、Windows Release、原生 CTest 和浏览器门禁。缺少的硬件、网络或生产环境继续明确标为待验收，不以本地回环或模拟回执替代。

## 4. 执行顺序

当前执行拆单以 [2026-11-20 分支基线与下一步计划](branch-baseline-and-next-actions-2026-11-20.md) 为准：先复核现有未提交实现与探针隔离，再复测并修复 C03 音频连续性，补真实媒体及显示场景，随后实现多连接标签页、H03 队列恢复和 C04。W1 环境验收继续保留，双机按既有安排后置。

1. 完成 W1 外部验收并汇总剩余缺口；环境未就绪时继续补齐可验证的测试工具、失败路径和文档，不虚报发布门槛通过。
2. W1 外部验收仍未收口；按 2026-11-20 用户决定，双机验证后置。C01 代码与本机自动化门禁已复验，继续保留真实电源行为、多会话实机释放及 W1 外部验收待办；核对默认仅查看偏好、会话保活、LAN 设置与 Strategy 的单一配置语义。
3. C03 对干净链路与默认弱网各做至少 5 轮同版本交替播放测试，汇总 starved、dropped、underruns 和 skew 分布并冻结连续性门槛；当前单次样本已显示弱网多出约 7.77 秒空转。随后用可控语音和下行参考验证真实麦克风可懂度及 AEC/降噪效果、权限撤销；再完成 C02/C03 的混合 DPI、旋转、全屏、多屏及物理显示器热插拔验收。达标后再推进 C04 和 H03 跨重启队列。
4. 并行开展 A01 密码同步安全设计和 O01/O02 生产部署设计；设计通过评审后再实现共享地址簿、多实例协调和生产数据库能力。
5. 随后实施 A02/A03、S04–S06、H01/H02/H03 及 O03/O04，最后执行 W5 发布候选门禁。

新发现的账号隔离、越权、数据丢失、错误断开或核心连接阻断问题优先于功能扩展。

## 5. 范围决定

macOS、Linux、Android、iOS 完整产品化，官方 RustDesk hbbs/hbbr 无修改客户端兼容，远程打印、Switch Sides、AV1 4:4:4、软件 VP8/AV1、静默自动更新、MaxMind 自动地理选路和 LDAP 组同步不进入当前默认范围。改变这些取舍需要先更新产品决定并单独立项。

## 6. 证据入口

- [分支基线与下一步执行计划（2026-11-20）](branch-baseline-and-next-actions-2026-11-20.md)
- [功能完成情况与后续计划](feature-status-and-plan-2026-11-16.md)
- [RustDesk 对照审核执行方案](rustdesk-execution-plan-2026-11-16.md)
- [W1 2026-11-20 当前状态与后续计划](w1-status-and-next-plan-2026-11-19.md)
- [W1/S01–S02 原生进程验收](native-runtime-acceptance-2026-11-17.md)
- [W1/S01–S02 管理台联动验收](strategy-connection-console-acceptance-2026-11-17.md)
- [S03 角色管理台完成状态](s03-role-console-status-2026-11-17.md)
- [S03 PostgreSQL 验收](postgres-console-acceptance-2026-11-17.md)
- [S03 角色并发保护验收](role-concurrency-acceptance-2026-11-17.md)
- [S03 删除、解绑与交互验收](role-actions-acceptance-2026-11-17.md)
