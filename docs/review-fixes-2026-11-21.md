# 2026-11-21 分支审核问题修复

归档前基线：`688638a`，分支 `codex/complete-workspace-2026-11-20`。本记录区分源码修复、本机验证及仍需专用环境的验收；其中按日期记载的未提交状态均是当时快照。2026-11-23 归档将本记录及对应源码/测试纳入当前分支快照，不改变下文的验证范围和未完成项。

## 最新结果（2026-11-24）

### C03 Win32 关闭消息失败注入与双标签回归（2026-11-24）

运行 `tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -MultiWindow -MultiWindowCloseFailure`。首轮测试注入器使用 `HWND(-1)` 后观察到 `host-wm-close`/`close-all`，没有进入预期的 `PostMessageW` 失败回滚；这是无效句柄选择错误，未作为通过证据。改用非特殊值 `0xDEADBEEF` 后重新运行，Windows Release 构建及 Flutter/Go loopback 验收 **PASS**（50.86 秒）。证据：`build/native-runtime-20261124-193406-b320b956/run-243170596/`。

通过轮次先在真实 Flutter 双标签进程中调用真实 `PostMessageW`，对无效 HWND 触发失败；TSV 记录 `close-tab-post-failed` 与 `close-tab-post-rollback`。JSON 快照确认前后均为 2 个标签、2 个会话窗口、1 个可见窗口，选中项保持为 1，查看端进程仍存活。随后同一进程完成 3 轮正常选中标签关闭/重连，兄弟会话每轮保持活动，末尾精确断开通过。失败轮次及修正过程日志保留于 `build/native-runtime-20261124-193156-44d3cb76/run-1531887794/` 与上述通过目录。

补充回归：`ctest --test-dir build\root -C Release -R '^session_tab_model$' --output-on-failure` **1/1 PASS**；`cd server && go test ./internal/api -count=1 -timeout 10m` **PASS**（42.788 秒）；两份验收 PowerShell 脚本 Parser 和 `git diff --check` 均通过。

本验收只证明“无效 HWND 导致的真实 Win32 `PostMessageW` 失败”路径及状态回滚，不模拟消息队列满载或所有其它投递错误，也没有定位 11-22 历史查看端进程退出的原因；不能据此宣称 C03 关闭异常整体解决。双机、物理显示/音频、电源及正式服务验收仍待。

### C03 正常关闭路径回归（2026-11-23 05:42）

在增加 `PostMessage(WM_CLOSE)` 失败回滚后，重新运行 `tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -MultiWindow`。Windows Release loopback 50.95 秒通过：同一 Flutter 进程完成 3 次选中标签关闭/重连，兄弟连接每轮保持 active，末尾管理台精确断开通过。轨迹 `build/native-runtime-20261123-054108-fd4b2f28/run-1570086336/session-tabs.tsv` 有 3 组 `close-tab-request`、`close-tab-post`、`prune-child`。累计 49 个历史失败后的关闭周期通过；本轮未触发投递失败分支，12:05/12:08 进程退出根因仍未定位。

### C03 子窗口关闭消息投递失败回滚（2026-11-23）

`SessionTabHostWindow::CloseTab` 现在保存关闭前的选中标签；若 `PostMessage(WM_CLOSE)` 明确失败且子窗口 HWND 仍有效，则撤销 closing 状态、恢复原选中项并同步可见性。若 HWND 已失效，继续由现有 prune 路径清理。成功投递时的关闭流程不变。新增模型回归覆盖选中项恢复、关闭最后一个活动标签后的恢复及恢复后可再次关闭；Release `session_tab_model` CTest 1/1 通过，Flutter Windows `chast_viewer` Release runner 重建通过。2026-11-24 已通过真实 Flutter loopback 对无效 HWND 注入 `PostMessageW` 失败并验证 host 集成回滚；该结果不解释 11-22 两次历史进程退出，C03 历史根因仍未定位，不能宣称关闭问题已整体修复。

### H03 主机入站归档服务端 API 第一阶段

新增 `/v1/hosts/{id}/recordings/uploads` 设备签名分片上传接口，复用账号归档的 quota、idle-session reclaim、幂等键、SHA-256 完成校验和保留策略。请求需主机 API 凭据与注册设备 Ed25519 签名；服务端按当前 registry owner 绑定账号，拒绝无 owner、封禁 owner、禁用设备和 owner 变更后不可用的旧上传。分片 SHA-256 纳入签名，服务端核验失败会回滚文件到原 offset。主机端不接受请求体指定账号。

`go test ./internal/account ./internal/recordings ./internal/identity ./internal/api -count=1` 全部通过。额外覆盖远程明文 HTTP 拒绝、loopback HTTP 放行、账号库持久 owner 绑定、owner 变更后 begin 冲突，以及两个 SQLite store 并发争抢同一设备任务键时只能有一个账号获绑定。`StreamRecorder` 基于复用器完成列表即时发布已封口分段；Release `mp4_recorder` 回归覆盖轮转、消费者抛错后本地文件保留和 I/O 失败不通知。Go tunnel 新增默认关闭的持久任务 worker，模拟服务端已收分片但响应丢失后恢复 offset，并以子进程验证跨进程锁；agent 包测试与 vet 通过。本小节记录的第一阶段结束时，**H03 主机归档仍未完成**：C++ 清单 producer、主机 opt-in/队列配置传递和 agent 实际 loopback 端到端仍待。该阶段后续进展及当前验收见下节；实现契约见[H03 设计](host-recording-archive-design-2026-11-23.md)。无服务安装、防火墙或真实 ProgramData 写入。

### H03 编译态 tunnel 父子进程验收复核（2026-11-23）

首次父子进程 E2E 失败是夹具完整性标签不匹配，不是 tunnel 子进程残留：`build/root` 继承 Windows Low Mandatory Integrity，C++ harness/tunnel 以 Low Integrity 运行，而 Go 测试在 `%TEMP%` 创建的任务文件完整性更高，因此 `os.Remove` 被 Mandatory Integrity 的 No-Write-Up 拒绝。验收现改为 C++ producer 在同一隔离构建目录生成父子进程用的大文件任务；未改 ACL 或 token，也未触碰正式服务配置。完整 `tools/test-host-recording-archive-e2e.ps1` 连续两次通过，覆盖真实编译态 tunnel、C++ `TunnelProcess` 父进程强杀后子进程回收、服务端 4 MiB offset 恢复、归档 SHA-256、清单删除及源 MP4 保留。Release CTest 4/4、`go test ./internal/agent ./cmd/chast-tunnel -count=1` 和 `go vet ./internal/agent ./cmd/chast-tunnel` 通过。

### H03 真实 chast-host 入站录制整链（2026-11-23 04:37）

新增 Windows opt-in 进程验收：在 `build/test-temp` 隔离目录启动测试构建的便携 `chast-host` 和它管理的真实 `chast-tunnel` 子进程；主机只绑定 loopback，配置开启 `record_incoming`/`upload_recordings`，并从当前用户证书库读取已有证书指纹。测试专用捕获器生成 160×120 合成 BGRA 图样，软件 VP9 正常编码；独立固定指纹 Desktop probe 收到五帧后结束会话。断言服务端注册 owner 下完成一份归档，记录大小和 SHA-256 与源 MP4 完全一致，且主机源文件仍存在。`go test ./internal/api -run '^TestWindowsNativeHostRecordingArchiveAcceptance$' -count=1 -v` 及完整 `tools/test-host-recording-archive-e2e.ps1` 均通过；完整入口还验证 C++ producer、tunnel 进程重启续传和 C++ `TunnelProcess` 父子进程恢复。主机录制与归档文件为 2278 字节。测试夹具同时要求测试构建宏、`CHAST_NATIVE_TESTING=1`、专用 opt-in、便携非正式 loopback 配置，且配置/录制/队列路径严格位于本次隔离根目录；没有既有身份时跳过，不创建证书。未捕获桌面、不安装服务、不操作真实 ProgramData/防火墙。正式生产/双机验证及源 MP4 清理策略仍待。

### C03 十轮关闭/重连压力复验（2026-11-23 05:24）

`native_runtime_windows_test.go` 新增仅用于 loopback 验收的 `CHAST_NATIVE_MULTIWINDOW_CLOSE_CYCLES`，默认 3、允许 1–20。设置为 10 后运行 `tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -MultiWindow`，Windows Release 构建及 `TestWindowsNativeRuntimeAcceptance` 通过（120.87 秒）。同一真实 Flutter 进程连续关闭/重连选中标签 10 次，兄弟连接每轮保持活动；末尾管理台精确断开通过。详细 trace `build/native-runtime-20261123-052137-c14aa6cd/run-779652209/session-tabs.tsv` 有 10 次 `close-tab-request`、`close-tab-post`、`prune-child`，20 次 `viewer-bridge-close`；无窗口集归零、标签宿主销毁或消息泵 quit/error。累计 46 个历史失败后的关闭周期通过。未复现旧故障，未获取退出码；C03 仍未修复，关闭失败回滚仍未验收，关闭行为未改。

### C03 runner 退出路径 trace 复验（2026-11-23）

为区分消息泵因窗口集合归零退出与外部/异常终止，在 `main.cpp` 增加仅测试 trace 开启时记录的 `runner-window-set-empty`、`runner-empty-tabs`、`runner-tab-host-destroyed`、`main-loop-quit` 和 `main-loop-error` 事件；其中空标签和宿主销毁事件在清理句柄前记录。该改动不改变窗口关闭或进程退出行为。修正后的 `tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -MultiWindow` Windows Release 构建及 loopback 通过，连续完成三轮关闭/重连，兄弟连接保持活动，最后的管理台精确断开也通过。轨迹位于 `build/native-runtime-20261123-050322-6bd81ab4/run-1018272889/session-tabs.tsv`：每轮均有 `close-tab-request`、`close-tab-post`、`viewer-bridge-close` 与 `prune-child`，没有窗口集归零、标签归零/宿主销毁或消息泵 quit/error 事件。旧进程退出未复现；本轮不能证明根因修复，C03 关闭冲突仍是未解决阻断，关闭失败回滚和进程异常终止来源仍待定位。

### C03 历史标签关闭退出复核（2026-11-23）

复查 2026-11-22 12:05/12:08 两次失败的原始目录：`session-tabs.tsv` 均在两会话已建立后记录 `close-tab-request` 和 `close-tab-post`，计数从 2 降到 1；之后没有子窗口 `WM_CLOSE`/`WM_DESTROY`、`viewer-bridge-close`、prune 或 host-close 事件，查看端顶层窗口清单为空。12:08 的关闭脚本明确记录 `process_alive=False`；脚本只点击关闭按钮并查询窗口，不会终止被测进程。旧版 Go 验收只返回脚本退出错误，没有稳定收集查看端退出码或 dump。只读检查本机 2026-11-22 12:00–12:15 Application 事件日志，没有查看端 Application Error/WER 记录；该区间只有无关的 Security-SPP 事件。另检查 WER 归档，找到两份 `chast_viewer.exe` 报告，时间为 2026-11-21 21:00:41/45（北京时间），异常码 `c0000005`/`c000041d`，故障模块均为 `flutter_windows.dll`、偏移 `0x12e3a`；报告 ID 与窗口初始化回归留存的 `crash-events.json` 一致，不是 11-22 两次标签关闭失败的时间段。现存证据无法区分 11-22 进程退出是应用主动退出、外部终止或未被 Windows 记录的异常，因此关闭根因仍未定位，未修改关闭行为。

为避免 Go `cmd.Wait()` 通知与 PowerShell 失败返回之间的调度竞态，验收器现在在关闭失败后最多等待 2 秒收集进程状态，并记录退出码十进制/十六进制及 wait error。新增 `TestNativeProcessExitStatusIncludesCode` 用退出码 37 的短进程验证 `0x00000025` 输出，定向测试 0.117 秒通过；`go test ./internal/api -count=1` 39.605 秒通过，`go vet ./internal/api` 通过。这仅改善下次复现的诊断，不能回溯恢复两次旧失败的退出码，也不能作为根因修复证明；之后累计 46 个关闭/重连周期通过仍不等同于修复。

### C03 标签关闭三批 trace 压力复验（23:48）

在 23:41 trace-disabled 三轮通过后，启用 `CHAST_NATIVE_TEST_LIFECYCLE_TRACE=1` 连续运行三个独立 `-MultiWindow` Release loopback 批次。每批均完成三轮“关闭选中标签→兄弟连接保持 active→重连关闭的标签”，并通过末尾管理台精确断开；三批 Go 验收分别 57.40、51.99、51.00 秒 PASS。三份 `session-tabs.tsv` 均各有 3 个 `close-tab-request`、3 个 `prune-child` 及 6 个 `viewer-bridge-close` 事件，查看端进程在每批验收期间保持存活。

证据目录：`build/native-runtime-20261122-234512-64332027/run-4193874309/`、`build/native-runtime-20261122-234624-b6c7a031/run-406761616/`、`build/native-runtime-20261122-234730-8cc8cf40/run-4281782606/`。累计 27 个后续实际关闭周期通过。12:05/12:08 两次历史进程退出仍未复现或定位；本轮没有据此修改关闭行为，关闭失败回滚仍待验收。

### C03 标签关闭 trace-disabled 复验（23:41）

移除 `CHAST_NATIVE_TEST_LIFECYCLE_TRACE` 后，以当前工作区重建并运行 `tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -MultiWindow`。Release loopback 连续三轮“关闭选中标签→兄弟连接保持 active→重连关闭的标签”全部通过，末尾管理台精确断开也通过；`TestWindowsNativeRuntimeAcceptance` 用例 50.98 秒 PASS。证据：`build/native-runtime-20261122-234008-86a4c7b9/run-674030585/`，其中 `session-tabs.tsv` 每轮均有 `close-tab-request`、`close-tab-post` 和 `prune-child`。累计 18 个后续实际关闭周期通过。

本轮未启用详细 runner trace，因而没有新的 child `WM_CLOSE`/消息泵事件轨迹；也未复现 12:05/12:08 的进程退出，不能据此判定根因已修复。关闭逻辑保持未改，关闭失败回滚仍待验收。

### C03 标签关闭复验（23:28）

在上一批累计 12 个后续实际关闭周期的基础上，最新 `-MultiWindow` Windows Release loopback 再连续三轮完成“关闭选中标签→兄弟连接保持 active→重连关闭的标签”；管理台精确断开也通过。最新 trace：`build/native-runtime-20261122-232717-55f31d69/run-8867425/session-tabs.tsv`，每轮均记录 child `WM_CLOSE`、Dart 关闭确认、`WM_DESTROY`/`WM_NCDESTROY` 和 `prune-child`。累计 15 个后续实际关闭周期通过；此前 access-token 前置认证失败未进入关闭步骤，不计入。

验收 runner 的 `GetMessage` 错误现在返回失败退出码，修复随本轮 Release 验收通过。12:05/12:08 的查看端进程退出仍未定位；后续未复现不能证明根因已消失，关闭逻辑未改，关闭失败回滚仍未验收。

### C03 标签关闭压力复验（23:18）

为诊断 12:05/12:08 的查看端进程退出，给 `-MultiWindow` 验收增加连续 3 次“关闭选中标签→兄弟会话保持 active→重新连接关闭会话”，并在关闭失败时附带查看端退出码。Windows Release loopback 三轮关闭/重连全部通过，末尾管理台精确断开也通过；产物 `build/native-runtime-20261122-231717-cbb83842/run-2586680336/`。此前另有 6 个单轮关闭/重连成功；`build/native-runtime-20261122-231203-03f61058/` 在首次连接阶段因测试 access token 无效而失败，未进入关闭步骤，不计入 C03 关闭失败。

为减少文件写入对关闭时序的影响，详细 runner/bridge 消息轨迹由 `CHAST_NATIVE_TEST_LIFECYCLE_TRACE=1` 显式开启。最新 trace-enabled 三轮压力运行也通过，产物 `build/native-runtime-20261122-232230-19e94508/run-3604000455/session-tabs.tsv`；三轮分别完整记录 `close-tab-post`、child `WM_CLOSE`、Dart `closeSessionWindow`、第二次 `WM_CLOSE`、`WM_DESTROY`、`WM_NCDESTROY`、`prune-child`。此前两次退出失败仍没有对应的 child/runner 轨迹或进程退出码，来源依旧未知。累计 12 个后续实际关闭周期通过，但**C03 关闭缺陷仍未定位，不能把后续未复现等同于修复；关闭逻辑保持未改，关闭失败回滚验收仍未通过。**

### C04 快捷键 Release loopback 复验（22:43）

22:04 首轮捕获关闭后的 Flutter 快捷键失败，已由本轮实际 Windows Release loopback 复验覆盖：Ctrl+F7 开关远控、原生捕获期间 Ctrl+F8 切换统计、关闭远控后普通 A 不发送到主机、关闭原生捕获后 Ctrl+F8 经 Flutter 回退切换统计，全部通过。机器可读结果中上述断言均为 true；证据为 `build/native-runtime-20261122-224253-d0c0572b/acceptance.log` 和 `run-1962698155/flutter-input-results.json`。

最终修复后的快捷键 Flutter 定向测试 **12/12 PASS**，`flutter analyze --no-pub` 无问题，Windows 键盘 CTest **1/1 PASS**。随后在最终工作区运行 `D:\flutter\bin\flutter.bat test --no-pub`，**870/870 PASS**。Release loopback 通过；此前记录的 Flutter 全量 **869/869** 与产品 Release 构建是在最后一次 hook 修复前。物理键盘/IME、真实应用间剪贴板互通和物理触控板仍待验收。22:04 失败日志保留作历史证据，不再视为当前快捷键缺陷。

### C04 可配置快捷键

快捷键现具备完整代码路径：支持切换远程控制、全屏、相对鼠标、统计面板和结束会话；配置按服务器/设备偏好保存，可录制、冲突拒绝、清除或取消。特殊按键菜单在仅查看状态仍允许打开配置，而真正的远程动作继续受输入授权限制。Windows 原生键盘 hook 负责主路径，并对匹配按键的重复/释放沿保持本地；带 revision 的窗口消息会丢弃设置变更前排队的旧动作。hook 不可用时由 Flutter 主会话焦点路径回退；本地 IME 模式在缺少原生 hook 时不拦截文本按键。

Flutter/native 两侧都拒绝 Win/Escape 键和 Alt+Tab、Alt+F4、Ctrl+Shift+Esc、Ctrl+Alt+Tab、Ctrl+Alt+Delete、IME/窗口切换及紧急相对鼠标退出组合；不接受 Right-Alt/AltGr 与 Win 修饰符。原生桥逐项检查 action、HID usage、修饰位及重复 action/chord，避免偏好文件或通道参数绕过 UI 校验。动作仅在活动窗口/标签、会话存活及相应权限允许时执行；录制快捷键期间暂停原生捕获，取消不写偏好。

验证：Flutter 全量 **869/869 PASS**（`flutter test --no-pub`，在最后一次原生 hook 修复前），`flutter analyze --no-pub` 无问题；快捷键/工具栏/偏好定向测试、MethodChannel 双向路由与 AltGr 回归通过；Windows 键盘 CTest **1/1 PASS**；Flutter Windows Release 产品构建 **40.0 秒 PASS**。2026-11-22 22:04 的 Windows Release loopback 曾未通过 Flutter 回退路径，诊断及失败产物仍保留于 `build/native-runtime-20261122-220340-4c69e343/`。后续 22:43 的复验已覆盖该失败，结果与最终修复后测试范围见本节前方的“C04 快捷键 Release loopback 复验”。**物理键盘/IME 的人工 Windows 实机验收仍待**；本轮未更改 C03 标签关闭路径，未安装服务或修改防火墙/ProgramData。

### C04 SVG 剪贴板与无效格式回退（20:30）

剪贴板内容新增 `ClipboardFormat::Svg = 6`，CCLB v4→v5、协议 v77→v78；SVG 单项上限 16 MiB，总内容预算由 112 MiB 增至 128 MiB，QUIC 流与待处理预算继续覆盖最大合法内容。Windows 以 `image/svg+xml` 读写；发送和接收均用 XmlLite 校验完整 SVG，拒绝 DTD、脚本/动画/扩展元素、事件/style 属性、外部链接和资源，只接受安全的本地片段引用。XML 深度、节点及属性数量均有上限。

验证期间原生探针暴露出一个真实回退缺陷：活动 SVG 与有效文本/PNG 共存会令整个剪贴板读取失败。读取逻辑现把 SVG 视为可选图片表示；只有安全有效的 SVG 才进入 CCLB，坏 SVG 不再抹掉其它有效格式；若坏 SVG 是唯一图片，仍返回失败而不建立空白基线。隔离的 Medium integrity 私有窗口站探针复核安全 SVG 往返、活动 SVG 单独拒绝、活动 SVG 与纯文本/PNG 混合时仅保留完整的 Text+PNG、PNG 格式别名、超限 DIB 转 PNG 和损坏 PNG 的 DIB 回退，全部通过。CMake Release 构建成功，`protocol` 与 `clipboard_async` 定向 CTest **2/2 PASS**；双向 ViewerEngine/HostServer/QUIC SVG 往返及 CCLB 恶意/超限用例均覆盖。验证日志：`build/test-temp/c04-svg-fallback-private-clipboard.log`、`c04-svg-focused-ctest.log`。

Flutter Windows Release 产品构建也通过（237.6 秒）；`flutter analyze --no-pub` 无问题，Flutter 全量 **861/861 PASS**。构建保留既有 C4456、UNICODE/`_UNICODE`、`getenv`、LNK4098/LNK4199 告警。该构建确认产品 CMake 路径成功编译并链接 XmlLite。私有 native 探针和 QUIC 测试不代表两个实际 Windows 应用在剪贴板中同时公布/消费 SVG 的兼容性，真实应用间 SVG 复制/粘贴仍待；输入法和真实键入验收也仍待。此处“下一项为可配置快捷键”为当时记录，后续实现和验证见本文件开头。本轮没有触碰 C03 标签关闭路径、服务安装、防火墙、ProgramData 或用户交互桌面剪贴板。

### C04 画布自动/边缘滚动与滚动条样式（20:00）

“视图与光标缩放”设置新增滚动条、自动滚动、边缘滚动三种样式；边缘感应范围 20–150 px、默认 100 px。滚动偏好按连接目标保存，主会话及显示器子窗口一致读取。原始大小或缩放后发生溢出时可平移；远程控制期间普通滚轮只发给远端，Shift+滚轮用于本地画布平移；拖动本地画布后重新投影鼠标坐标，避免按下/移动位置仍使用滚动前的旧坐标。边缘滚动通过帧计时器依指针距边缘的距离连续平移，离开视图、按下输入、失焦/挂起或模式变化都会停止。

新增回归覆盖自动滚动坐标重映射、静止指针边缘连续平移、边缘模式下滚轮仍仅路由到远端、样式偏好保存/非法值回退和显示子窗口设置。Flutter 全量 **861/861 PASS**，`flutter analyze --no-pub` 无问题，Windows Release 构建 PASS（38.7 秒）。物理触控板、真实触摸屏、混合 DPI 和跨设备验收仍待；自动滚动的实际交互手感需在 Windows 实机确认。SVG 剪贴板后续修复与验证见本文件最新结果；可配置快捷键仍未实现。C03 标签关闭路径未修改。

### C04 触控板双指平移速度倍率

查看端“鼠标”菜单提供触控板速度滑块，范围 10%–1000%、步进 10%、默认 100%；设置按服务器+主机组成的设备连接目标保存，并供主会话和显示器子窗口共同读取。倍率只缩放触控板双指平移送往远端的 wheel delta，捏合缩放、鼠标滚轮和默认手感不变；取消对话框不写入设置。

定向偏好、触控板手势、工具栏及速度对话框用例通过；该切片当时全量 Flutter **857/857 PASS**，`flutter analyze --no-pub` 无问题，Windows Release 构建 PASS。覆盖默认/非法值回退、设备连接目标隔离、对话框应用/取消、50%/200% 平移及缩放保持。Windows 物理触控板实机验收仍待；边缘滚动/样式后续结果见本文件开头。本轮未修改 C03 标签关闭路径。

### C04 从本地剪贴板按键发送

主会话“发送文本”对话框增加本地纯文本读取按钮。只有用户点击才读取，读取后仅填入编辑框；用户确认才走现有 `ViewerBridge.sendText`，继续由原生查看端和主机检查远程输入权限，不依赖剪贴板同步。保留 4096 UTF-8 字节上限和空字符校验；空、超限和读取失败不覆盖草稿，异步读取期间也不覆盖较新的编辑。

该剪贴板切片当时的定向 Flutter 对话框/本地化测试 **15/15 PASS**，全量 **852/852 PASS**，`flutter analyze --no-pub` 无问题。用例覆盖中文、emoji、换行、空白、确认/取消、非法内容和延迟读取。真实 Windows 剪贴板到远端键入及输入法仍待；边缘滚动/滚动样式和 SVG 格式后来已有实现，详见本文件开头；可配置快捷键仍未实现。C03 标签关闭路径未改。

### H03 查看端录制上传队列恢复（本轮）

查看端现以 `%APPDATA%\\chast\\recording-uploads.json` 保存待上传任务，写入采用临时文件替换；任务绑定规范化服务器地址和账号名，保存录制元数据及服务端上传会话 ID，不保存 bearer token。队列操作有文件锁，上传 worker 使用独立锁文件避免多个查看端进程并行处理同一队列；队列文件/录制文件在上传期间受保留策略保护。会话启动后会 drain 匹配当前账号的任务，未匹配账号的任务留在队列；失败后保留任务并重试。Flutter 回归覆盖 store 重建、账号/服务器隔离、worker lease、损坏队列不覆盖及会话打开后的恢复；队列/上传/服务器 URL 定向测试 **21/21 PASS**，Flutter 全量测试 **836/836 PASS**，Flutter analyze 无问题，Go `internal/account`、`internal/recordings`、`internal/api` 定向测试 PASS。

新增纯 Dart `server_url.dart`，使测试探针直接调用生产 `RecordingUploadQueueStore`，不必在子进程启动 Flutter UI runtime。`tools/test-recording-upload-worker-lock.ps1` 已通过：队列与 upload ID 由独立 Dart 进程写入、另一进程读取；持有 worker lock 时第二进程被拒绝；owner 正常退出和被强制终止后，锁都可重新获取，强杀后已保存的 upload ID 仍在。该测试证明 Dart 文件锁/队列存储的跨进程行为，但不是实际 Flutter UI/HTTP 上传进程的端到端强杀验收。策略测试改用各自临时队列，teardown 卸载页面并等待 worker 释放后才删除测试目录，避免接触真实 APPDATA。

本轮 analyze 暴露并修正了队列构造器的 initializing-formal 风格问题。损坏队列会显式报错而不静默覆盖。未做服务端部署或双机验收，也未提交、推送。

### 2026-11-22 H03 显示子窗口预写入与恢复

修复显示子窗口录制未进入持久上传队列的缺口。主会话只向子窗口传递服务器、用户名、设备/Peer 标识等非令牌元数据；子窗口开始录制前先将 intent 写入队列文件，再把预留 MP4 路径交给 native `setRecording`。父会话观察到共享 ViewerEngine 的录制路径后，按规范化文件路径找到并认领原 intent/lease；录制完成时沿用同一任务 ID 和归属元数据入队，避免重复任务。令牌仍由父会话从本地账号会话解析，不持久化到队列或传给子窗口。

队列恢复和 drain 现在仅在本次会话有效上传策略开启时执行；关闭上传时不扫描全局 APPDATA 队列，也不阻塞普通会话启动。回归验证覆盖子窗口调用 native 前意图已存在、父会话按路径认领、队列/上传恢复及显示子窗口媒体控制：定向测试 16/16 PASS，Flutter 全量 **841/841 PASS**，`flutter analyze --no-pub` 无问题，Windows Release 构建 PASS（66.6 秒；仅既有 LNK4098/LNK4199 告警）。

**H03 仍未完全关闭**：实际 Flutter UI/HTTP 上传进程强杀并重启的端到端验收尚未做；现有被强杀验收只覆盖独立 Dart 队列/worker 锁探针。应用冷启动后若不打开会话，队列不会自动 drain。未封口 MP4 及其 intent 会保留等待后续重试，尚无用户提示、过期/清理策略；不得误删或上传损坏文件。未做服务端部署或双机验收，也未提交、推送。

### 2026-11-22 H03 编号分段恢复

录制意图恢复现由 `RecordingUploadQueueStore` 执行：从 intent 的基础路径扫描 `base-N.mp4` 同组分段，仅将结构完整、非空的 MP4 幂等写入上传队列；队列按规范化路径及账号去重。所有分段都已封口后删除 intent；若任一分段未封口，则保留 intent，后续恢复可补入新封口分段而不重复排队已有文件。页面仍只在上传策略开启时触发恢复。

验证：队列层恢复/分段用例 **8/8 PASS**，会话上传集成 **8/8 PASS**，Flutter 全量 **843/843 PASS**，`flutter analyze --no-pub` 无问题，Windows Release 构建 **33.1 秒 PASS**。这些是本机自动化和构建证据，不代表 Flutter UI/HTTP 进程被强杀后的端到端恢复。

**剩余 H03 验收**：实际 Flutter 上传进程强杀后重启并自动恢复仍未验证；应用冷启动后若用户不打开会话，队列不会自动 drain。未封口 MP4 会被保留，但尚无用户提示及安全的过期/清理策略。服务端部署和双机验收仍后置；本轮未安装服务、修改防火墙/ProgramData，也未提交或推送。

### 2026-11-22 H03 首页冷启动 drain

首页初始化完成账号/代理加载与账号续期后，会在没有打开会话的情况下启动共用上传 worker；登录成功及启用录制上传时也会请求 drain。队列仍按当前账号隔离，逐 host 解析 Viewer Strategy 后才恢复 intent 或上传任务；worker lease 防止多个查看端进程并行上传。首页 widget 回归覆盖“无会话启动会调用 worker”和“本地策略关闭时不授权上传”。

本轮 H03 worker/会话上传定向测试 **10/10 PASS**，`home_page_test.dart` **65/65 PASS**；Flutter 全量 **847/847 PASS**，`flutter analyze --no-pub` 无问题，Windows Release 构建 PASS（33.2 秒）。因此此前“无会话冷启动不自动 drain”的记录已被代码及 widget 自动化覆盖并由本节更新；测试使用 fake worker，尚未证明实际 Flutter UI/HTTP 进程强杀后重启恢复。未封口 MP4 仍保留，但缺少用户提示及安全过期/清理策略。服务端部署和双机验收仍后置；未安装服务、修改防火墙/ProgramData，未提交或推送。

### 2026-11-22 H03 实际进程恢复与未封口文件策略

`tools/test-recording-upload-app-restart.ps1 -TimeoutSeconds 30` 使用真实 Flutter Windows Release 进程和仅绑定 `127.0.0.1` 的隔离 HTTP fixture。服务端已接收首个 PATCH 数据但挂起响应时，脚本确认 upload ID 已持久化并强制终止首进程；第二个实际 Flutter 进程请求服务端当前 offset、完成归档，fixture 验证全部 44 字节与 SHA-256 `c88cc81a20a6afbbe8905c05522cb1e6545bcf573bdde9273c149ef02d895546` 一致，本地任务最终为空。脚本保留 build target、HTTP trace、两进程 stdout/stderr、隔离队列及 fixture 于 `build/test-temp/chast-upload-app-e3ff6e4998fc45279fcd19ea5aee56a1`，并在退出时重新构建默认产品 Release。没有安装服务、改防火墙或写入真实 `%ProgramData%`。

未封口 MP4 的策略现为明确的保守保留：首页显示未封口分段数；每分钟及上传 drain 后以非阻塞队列锁刷新状态，争用/不可读时显示检查告警而不延迟上传。未封口媒体和恢复 intent 不设置自动过期，不自动上传或删除；所有分段封口后沿用既有幂等恢复入队并移除 intent；只有录制文件已不存在时才移除无内容可恢复的 intent。队列单测验证分段由未封口转为封口前后状态变化和文件保留，首页 widget 验证提示可见及文件/intent 未删除。

本轮 Flutter 全量 **848/848 PASS**，`flutter analyze --no-pub` 无问题，Windows Release 构建 PASS（48.8 秒）；进程强杀重启验收 PASS，定向队列/首页/本地化回归通过。由此关闭 H03 **查看端恢复延伸**的本机代码与验收缺口。H03 原计划中的主机入站集中归档尚属独立功能缺口；服务端部署/双机按用户安排后置，W1 外部验收、C03 标签关闭冲突和真实音频/物理显示验收仍未关闭。本轮未提交或推送。

### 12:08 C03 标签关闭冲突复现（仅诊断，未改关闭路径）

以 `tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -MultiWindow` 重跑。连续两次最新运行都确认原生关闭按钮命中并记录 `close-tab-request`、`close-tab-post`（选择从 2 个标签变为 1 个活动标签），之后没有 `prune-child` 或宿主关闭轨迹；UI 验收 15 秒后看到 `count=0, windows=0, visible=0`。12:08 运行的失败诊断进一步记录查看端进程已退出（`process_alive=False`），`EnumWindows` 没有该进程顶层窗口；无匹配的 Windows Application crash 事件。轨迹及窗口树快照：`build/native-runtime-20261122-120816-e7534627/run-32384536/session-tabs.tsv`、`flutter-window-inventory-after-close-failure.txt`；前一轮轨迹：`build/native-runtime-20261122-120536-a19d6fdb/run-2777428185/session-tabs.tsv`。

这排除了“仅因脚本短暂枚举到 0 个标签”的解释，但还不能确定哪个组件发出了进程退出信号；没有子窗口 `WM_DESTROY`/宿主关闭事件，Windows 错误日志也不能证明退出来源。标签关闭逻辑按用户约束保持未改；关闭隔离/回滚验收仍失败，作为未解决阻断项保留。验收脚本只增加失败时进程存活与窗口树快照。

### 11:53 C03 显示子窗口父会话生命周期 Release 验收

`tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -MultiWindowDisplay` PASS（Flutter Release 构建及 Go acceptance；用例 11.81 秒）。同一 Flutter 进程建立 A/B 两个 loopback 标签，合成 host 公布两个逻辑显示器并将其只读映射到同一台本机桌面捕获；实际打开两个显示子窗口，切换标签确认各窗口随父标签显隐，再断开 A。UI 断言确认 A 子窗口关闭、B 子窗口仍可见且兄弟标签存活；native 状态确认 A=`ended`（state 4），B=`streaming`（state 3），B 解码计数 1→115。证据：`build/native-runtime-20261122-115352-3e16ff0f/flutter-display-window-163144787/flutter-display-window-lifecycle.json` 及同目录 UI 截图、TSV 和日志。

首轮验收 UI 通过但 Go 状态断言超时，原因是测试状态 TSV 只按周期写入，ViewerEngine 退出前最后一份仍显示 `streaming`。现在仅在 `CHAST_NATIVE_TESTING=1` 的既有门控下，于 ViewerEngine 收尾后写一次终态快照；重跑通过。该结果证明合成双显示器拓扑下真实 Flutter Release 窗口与父会话的单机 loopback 生命周期，不证明物理多显示器、热插拔、DPI/旋转/全屏或双机行为；没有改标签关闭逻辑，也未安装服务或改系统显示配置。

### 11:10 C01 多标签电源租约与 C03 静音验收收口

专用 Release loopback 验收现已覆盖两条真实 Flutter 会话的保活租约生命周期。修复中发现，连接进入 ended/failed 后页面停止轮询并保留结束卡片，但直到页面关闭/重连才释放 lease；现在首次观察终态就调用幂等释放方法，teardown 复用同一方法。验收轨迹为初始 2/2 held；首条断开后该会话 `state=ended, held=false`、兄弟仍 `state=streaming, held=true`；末条断开后 0/2 held。证据：`build/native-runtime-20261122-111003-03433934/flutter-tab-audio-183818977/flutter-tab-awake-lease-isolation.json` 和 `awake-leases.tsv`。这证明 loopback 中真实 Flutter 会话/原生桥的请求生命周期，不等同于物理电源行为或跨机验收。

同轮真实 Flutter 工具栏双标签音频隔离通过：A 静音不影响 B、B 静音不影响 A、切回 A 取消静音后 B 保持静音，两路均继续解码。10:56 失败证据显示旧屏幕坐标 `width*0.57` 落在音量图标左缘附近，静音未触发；脚本改为优先按辅助标签找控件，并在该 Flutter HWND 不暴露按钮 UIA 节点时使用经截图核对的 `width*0.583` 坐标回退。11:10 结果记录 fallback `x=690`，静音隔离和租约断开顺序均通过。音频证据：同目录 `flutter-tab-audio-isolation.json`、`flutter-input-results.json` 和前后截图。仅使用 loopback 合成静音 Opus 与丢弃型播放器，未打开物理音频设备。

验证：`tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -MultiWindowAudio` Release loopback PASS（15.72 秒）；Flutter 全量测试 **829/829 PASS**；`flutter analyze --no-pub` **No issues found**；`display_menu_test.dart` **42/42 PASS**；Windows Release 构建包含于验收脚本并通过。一次中间运行已通过音频路由但因终态 lease 未释放而超时，修复后完整重跑通过。全量测试后仅将同文件 Semantics 测试的弃用断言 API 换为 `flagsCollection` 并完成 analyze/42 项复验。

本机双标签静音 UI 路由、首断/末断 lease 生命周期及合成双显示器拓扑下的父子窗口断开生命周期从“待验收”更新为“loopback 已验收”。真实 OS 电源行为、物理音频、语音/AEC/降噪、物理显示器/DPI/热插拔和双机仍待完成；标签关闭结果冲突仍按约束不改关闭逻辑。整体 C03、W1、W3/W4 和发布门槛未完成。

### C03 标签会话显示窗口归属回归

将 `SessionTabVisibility` 拆为独立 Win32 组件，新增 `session_tab_visibility` 原生测试。测试覆盖两个会话的显示子窗口随活动标签切换、仅活动会话拥有可见窗口、同一 HWND 重新分配到另一会话时替换旧归属、迟到的旧 owner 移除不能删除新归属、失效 HWND 在会话切换时清理，以及清空活动会话后隐藏全部登记窗口。重新分配路径此前会因只按 HWND 去重而保留旧 session 指针；现在会更新归属并立即按新会话活动状态同步可见性。

验证：`session_tab_visibility`、`session_tab_model`、`display_window_access`、`window_focus`、`reference_counted_lease`、`viewer_awake_request` CTest **6/6 PASS**；Flutter Windows Release 构建通过（18.3 秒），仅有既有 LNK4098/LNK4199 告警。随后双标签 loopback 已验证首个断开后兄弟继续持有 lease、最后一个断开后归零；11:53 专项验收已验证合成双显示器拓扑下父标签断开关闭其显示窗口且兄弟继续 streaming。以上都不替代物理多显示器/电源行为及双机验收。标签关闭实现保持未改，关闭结果冲突仍独立待处理。

### C01 拒绝结束会话后的迟到保活请求

原生 `setKeepAwake(true)` 之前只看本地 `keep_awake_` 标志，不检查 ViewerEngine 状态。如果 Flutter teardown 与连接刚结束的异步 continuation 交错，已结束会话仍可能新取得进程级 Windows 电源请求，而 Dart teardown 已经错过对应释放检查。现在将该调用归一为 `Noop/Acquire/Release/Reject` 策略：仅 `connecting`、`handshaking`、`streaming` 可 acquire；ended/failed/idle 拒绝新租约；已有租约可幂等保持，释放即使在 ended 状态仍允许。

`viewer_awake_request` 增加策略断言，覆盖活动会话首次 acquire、结束后迟到 acquire 拒绝、重复 acquire 不双计、结束后 release 仍可执行及无租约 release 为 no-op。与子窗口归属、标签模型、显示准入、焦点和引用计数回归合计 CTest **6/6 PASS**；Flutter Windows Release 构建通过（27.6 秒），仅有既有 LNK4098/LNK4199 告警。仍需真实双 Flutter 标签 UI 生命周期验收，确认 A 断开后 B 保持租约、最后会话退出后 OS 请求释放。

### 10:20 C03 双标签 Flutter 音频 UI 路由

新增专用 `-Flutter -MultiWindowAudio` Release loopback 验收。两个真实 Flutter 标签连接 loopback `--media-window-host`，主机仅发送合成静音 Opus；查看端通过仅在 `CHAST_NATIVE_TESTING=1` 且 `CHAST_NATIVE_TEST_AUDIO_SINK=1` 时启用的丢弃型测试播放器解码，不打开 WASAPI 输出。自动化对 A 执行静音时 B 保持未静音；切到 B 静音后 A 保持静音；切回 A 取消静音后 B 仍保持静音；各步骤及标签切回后均确认两路持续解码。最终 A/B 状态分别为未静音/静音，首次记录的解码计数 A 36→669、B 46→468。Release Go/UI acceptance 连续 3/3 PASS，分别用时 10.02、9.97、10.04 秒；音频按钮通过实际工具栏点击触发，原生每会话 TSV 状态作路由断言。最近证据：`build/native-runtime-20261122-102955-c7e0c926/flutter-tab-audio-3261278057/flutter-tab-audio-isolation.json`、同目录 `flutter-input-results.json`、A/B 前后截图、`viewer-status/`；前两轮证据位于 `build/native-runtime-20261122-102052-69d33a82/` 和 `build/native-runtime-20261122-102701-8db1c64a/`。这关闭“真实 Flutter 标签静音按钮路由”验收，不证明物理播放设备、真实声音或音质/AEC/语音效果。

### 09:51 C03 原生双会话音频隔离

扩展现有 `viewer_audio_video_isolation` CTest：两条独立 `ViewerEngine` 连接 loopback host 的合成静音 Opus 源，分别注入独立的内存播放器，测试完全不打开物理音频输出。A 静音/B 未静音时，两路均持续收包/解码，但只有 A 的内存播放器将新块计为静音；反向切换后 A 恢复、B 静音，状态与块计数仍各自独立。慢视频 5 FPS 下音频持续；断开 A 后 B 仍收流/解码；最后重连成功。`ctest --test-dir build/root -C Release -R '^viewer_audio_video_isolation$' -V` 1/1 PASS，7.94 秒：A/B 解码各 220/224 包，重连 117/117 包，视频帧 A/B 各 12，错误 0。Flutter Windows Release 构建 PASS，58.1 秒。此证据覆盖 `ViewerEngine` 后端隔离，不覆盖真实 Flutter 标签工具栏把静音点击路由到选中会话；Go UI fixture 仍为 `audio=false`。

### 09:29 C03 逐标签录制 UI 与媒体连续性

Release loopback 通过：A 保持 streaming 且不录制，B 保持 streaming/录制；B 隐藏 1,913 ms 时收包、解码、发布各 247→312，录制样本 209→273。A/B 工具栏截图的录制按钮高亮像素分别为 0/126，PowerShell 驱动和 Go 验收分别强制 A≤4、B≥20，并检查两张截图存在。切换按键释放、隐藏标签输入拒绝同轮通过。证据：`build/native-runtime-20261122-092823-23ee1ae7/run-166595522/flutter-tab-recording-isolation.json`、`flutter-input-results.json` 和两张 `flutter-tab-*-recording-button.png`。Flutter `display_menu_test.dart` 42/42 PASS；完整 `tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -MultiWindow -MultiWindowInput` PASS（Go acceptance 20.02 秒）。UI Automation 只能看到 `FLUTTERVIEW`，因此按钮通过工具栏截图像素验证，不伪称控件名/UIA 验证。

音频隔离代码检查：每个标签由独立 Flutter 子窗口和 `ViewerEngine` 承载；`audio_muted_`、输出设备选择及会话音频播放器均为该 engine/Run 的实例状态。通用多窗口 Go fixture 仍设置 `audio=false`；10:20 验收使用独立 synthetic-audio fixture 覆盖真实 Flutter 标签音量按钮路由。合成源和丢弃型播放器只证明 UI→目标会话状态路由及解码连续性，不证明物理播放设备或声学效果。

C03 多连接标签容器已从设计进入本地实现，并通过基础真实窗口切换回归。`tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -MultiWindow` PASS（Go acceptance 20.88 秒）：同一 Flutter 进程创建两条独立 loopback 连接，原生标签实际切换 `1 -> 0 -> 1`，每一步确认恰有一个会话子窗口可见；随后按目标 connection ID 精确断开 A，服务端确认 B 仍 active。证据：`build/native-runtime-20261122-062558-0470e5b3/run-3045181632/`。后续已新增独立 `SessionTabVisibility` 原生回归，覆盖会话归属和 HWND 重新分配；本机相关 CTest 6/6、Flutter Windows Release 构建通过。仍未证明真实多显示器子窗口的父连接生命周期，或真实 Flutter 双标签断开顺序与电源请求释放。此前证据仍不覆盖切换输入释放、后台输入拒绝、持续媒体及逐标签 UI（这些由 08:31、09:29、10:20 专项补齐）。前几次脚本尝试暴露跨进程 RECT 指针和 UI Automation provider 限制，失败产物仍保留在相邻 `native-runtime-20261122-*` 目录。

2026-11-22 07:07 的后续 Release loopback（30.81 秒）补验了选中标签关闭和重连：通过原生关闭按钮关闭一条连接，服务端观察该 ID 进入 `closed`、兄弟仍 `active`；首页重连生成新 connection ID，兄弟 ID 保持不变；随后精确断开新连接，另一连接仍 active。最终证据：`build/native-runtime-20261122-070706-f9b5c057/run-2007361905/`。首次测试器尝试直接发送 `WM_COMMAND` 误致容器关闭；改为通过真实按钮 HWND 发送 `BM_CLICK` 后验收通过，因此前次失败归于测试驱动，不作为产品关闭回归。该证据仍不证明兄弟持续收帧/录制或关闭失败回滚。native 活动视图焦点准入新增活动、隐藏、兄弟视图及错误前台的原生回归；`window_focus` CTest 通过。Windows Release Flutter 构建通过，保留既有 LNK4098/LNK4199 告警。

2026-11-22 08:31 的专用双标签 Release loopback（18.09 秒）通过：切出 A 时主机 dry-run trace 依次证明收到 `InputRelease`、排入释放、开始/完成释放，并在物理清理前注入 A-up；向隐藏视图发送鼠标消息后主机鼠标事件仍为 0。随后切回 A 并让 B 隐藏运行 1,867 ms，B 的状态快照仍为 streaming/recording；收包、解码、发布计数各从 253 增至 309，录制样本从 220 增至 276。证据：`build/native-runtime-20261122-083058-21a139b0/run-1332851354/`，包括 `flutter-input-results.json`、两个会话的 TSV 快照和 `input-events.jsonl`。这证明本机 loopback 中标签切换输入释放/后台拒绝及兄弟连接持续收流、解码、发布和录制；不替代双机、长时、真实设备或音频隔离验收。此前 08:18 的尝试因把录制中仍为 0 字节的 MP4 当作连续性断言而失败；本次改用引擎收流/解码/发布/录制样本计数，证明文件实时大小不是有效的录制进度指标。

2026-11-22 08:59 的双标签 Release loopback（18.10 秒）新增逐会话录制状态断言并通过：开启第二标签自动录制前先记录 A 的独立快照，A 保持 streaming、`recording=false`、样本为 0；B 保持 streaming、`recording=true`。B 隐藏运行 1,863 ms 时收包/解码/发布各 248→304、录制样本 215→271；结束快照仍为 A 关闭录制、B 开启录制，且 audit ref 与录制路径均不同。证据：`build/native-runtime-20261122-085831-bf256fe4/run-381049699/flutter-tab-recording-isolation.json`、`flutter-input-results.json` 和 `viewer-status/`。08:57 首次运行未进入隔离断言，定位为 Go 测试解析器 `TrimSpace` 删除了末尾空 TSV 字段；保留制表符后重跑通过。此结果证明原生会话录制状态隔离和隐藏标签录制进度，不证明 Flutter 录制按钮 UI 或音频播放/静音隔离。

**选中标签关闭结果存在冲突，暂不判定通过**：07:07 记录的通过证据保留；之后 07:59（`build/native-runtime-20261122-075852-87376bbe/`）和 08:04（`build/native-runtime-20261122-080431-499487b5/`）两次 Release loopback 都观察到关闭后标签状态无效、会话窗口数/可见窗口数为 0，未留下兄弟会话。按当前约束不改关闭逻辑；需要复现定位并补回归后才能关闭此项。

录制权限提示也已修正：当状态变化表明主机撤权停止录制，且 `recordError` 为旧版 native 的规范原因 `the host revoked recording` 时，查看端不再把它包装成“录制失败”，而显示本地化的“主机已撤销录制权限，录制已停止”；磁盘/编码等其它错误仍显示具体失败原因。主会话页和显示子窗口的 widget 回归及英语翻译回归通过；Flutter 定向测试 10/10、全量测试 828/828、`flutter analyze --no-pub` 和 `l10n_test.dart` 7/7 PASS。此处验证的是 Flutter 状态到提示的映射，不是新的 Windows Release 实际弹窗捕获；既有 Release 录制撤权验收仍只证明停止/恢复/关闭行为。

本次仍未提交或推送；未安装服务、修改防火墙/真实 ProgramData、采集真实麦克风或进行双机测试。

## 最新结果（2026-11-21 21:30）

音视频组合门禁的 10/10 结果保持下节所列范围。本轮继续发现并修复了两个实际问题，均仍属于本地未提交修改：

1. **录制权限撤销未停止正在进行的录制**。旧实现及测试注释声称该行为与 RustDesk 一致；核对 D 盘 RustDesk `55786aaa5709ac0a450aabcf06f9d1fee3119908` 的 `src/client/io_loop.rs` 后，确认其撤权会更新录制状态并停止录制。当前修复将开始录制的权限检查与撤权串行，并在录制器所属线程完成文件收尾；恢复权限不自动重启。新增 `viewer_recording_revocation` 修复前失败（9.43 秒，CTest 退出码 8），修复后与会话隔离、音视频隔离回归共 3/3 PASS（18.32 秒）。原始日志 `plan-record-revocation-{baseline,fixed}-20261121.log` 均保留。
2. **显示子窗口创建期间无障碍查询导致整个查看端崩溃**。21:00:41/21:00:45 的事件记录包含 `flutter_windows.dll + 0x12e3a`、`0xc0000005`/`0xc000041d`；本地 Flutter 3.44.0 源码与独立探针定位到新 `FLUTTERVIEW` 已创建但 delegate 尚未赋值时收到 `WM_GETOBJECT`。增加仅在当前线程控制器初始化期间生效的保护，初始化完成后恢复正常查询。独立基线探针失败，保护后通过；持久回归创建两个真实 Flutter engine，覆盖初始化查询、构造后查询及子引擎先销毁。该回归退出码 0，不能等同于全屏幕阅读器功能覆盖。

后续验证：Flutter analyze 无问题、录制状态/显示窗口偏好/录制上传定向 10/10 PASS；录制修复后的 Release 构建通过，窗口初始化保护后的最终 Release 构建再次通过（24.7 秒）。日志为 `plan-record-revocation-flutter-{analyze,test,build}-20261121.log`、`plan-flutter-window-init-test-20261121.log`、`plan-window-init-flutter-build-20261121.log`。窗口回归证据位于 `flutter-window-init-246fa89dd3de48028e410f55be6c03b9/`；旧崩溃记录保留于 `media-windows-38114b889de24567a7e386573008d37e/crash-events.json`。

**真实 Release 窗口复验已部分完成**：在隔离 APPDATA/USERPROFILE、仅回环连接和合成静音的环境中创建显示子窗口，从子窗口开始录制，主机撤权后按钮变为“主机未开放录制”并停止写入；恢复权限后回到“录制到本地”，没有自动重新录制。生成的单个 MP4 为 3,113,117 字节，顶层 `ftyp/mdat/moov` 结构完整，本文件未做全量解码。关闭子窗口后主会话与首页仍存活，随后全部测试窗口及 fixture 正常结束；复验时间段无匹配的 Windows 应用崩溃事件。该版本查看端 SHA-256 为 `6B4F278AEDEB987B31EEDB98049244B810070E4BE881B6901502658ECCB6582C`。证据：`build/test-temp/media-windows-fixed-d94d77f157084e9eb03ed9be57356154/` 下的 `ui-result.json`、`viewer-hash.json`、`crash-events-after-fix.json` 及同名目录旁的主机日志。

当时记录的待办由本文件开头 11:53 最新结果及“仍保留的功能与验收”表更新：双标签 lease loopback、逐标签静音/录制 UI 和合成双显示器父子窗口生命周期已通过；真实 OS 电源、真实音频/设备、物理多屏/混合 DPI/旋转/全屏/热插拔、完整窗口重连与独立第二连接 UI 隔离仍待验收。撤权提示已完成 Flutter 分类/本地化和 widget 回归，尚无新的 Release 实际弹窗采证。选中标签关闭/重连结果仍有冲突，失败回滚待定位；H03 跨进程上传队列与 C04 未实现。整个 C03、W1 外部验收及 W3/W4 仍未完成。本轮未采集真实麦克风，未安装服务或更改防火墙/真实 ProgramData，未提交、推送。

## 音视频组合门禁（2026-11-21 20:50）

候选 C 的干净/弱网各五轮组合门禁 **10/10 PASS，脚本退出码 0**。固定 `chast-quic.exe` SHA-256 为 `FFB5508203309B4B6143A784D5850A39C6C81D8187519120F7F159AC0A68B263`；十轮均核对二进制未变，期间未并行编译或运行其它媒体探针。使用 `tools/test-audio-continuity.ps1 -Rounds 5`，沿用下文候选 B 的网络参数、25 秒采样、音频预算和 3000 ms 视频门槛，没有放宽断言。

| 场景/轮次 | 累计空转 ms | 设备最大断档 ms | 丢弃块 | 最终 skew ms | 弱网最大视频帧龄 ms | 退出码 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 干净 1 | 0 | 6 | 0 | +13.3 | — | 0 |
| 弱网 1 | 1193 | 145 | 97 | +35.1 | 962 | 0 |
| 干净 2 | 0 | 7 | 0 | +18.3 | — | 0 |
| 弱网 2 | 1266 | 140 | 99 | +27.4 | 1731 | 0 |
| 干净 3 | 70 | 39 | 3 | +33.6 | — | 0 |
| 弱网 3 | 1230 | 164 | 97 | +31.0 | 1801 | 0 |
| 干净 4 | 0 | 8 | 0 | +15.9 | — | 0 |
| 弱网 4 | 1111 | 305 | 86 | +33.6 | 1759 | 0 |
| 干净 5 | 0 | 8 | 0 | +7.5 | — | 0 |
| 弱网 5 | 1047 | 271 | 86 | +37.6 | 1415 | 0 |

原始日志与机器可读结果：`build/test-temp/audio-continuity-1641192a53a343499ad0b5ce0423c114/` 下的 `clean-{1..5}.log`、`weak-{1..5}.log`、`results.json`；批次摘要 `plan-final-c-batch-20261121.log`。干净模式未输出视频最大帧龄汇总，表中不补造该数字。弱网仍有 140–305 ms 的最大设备断档及 86–99 个丢弃块，因此只关闭本组实验门禁，不能写成音频无断档或真实听感达标。历史候选 A/B 及后续三次失败保留如下。

同一修复版本的验证：

- 原生定向单测 9/9 PASS（4.63 秒）：协议、键盘、显示窗口准入、音频编解码、转发、DSP、播放、共享采集、视频 QoS；日志 `plan-final-c-unit-20261121.log`。
- 原生集成 4/4 PASS（189.28 秒）：`host_parity`、`desktop_stream`、`viewer_audio_video_isolation`、`quic_weak_network`；日志 `plan-final-c-integration-20261121.log`。其中普通弱网 CTest 不覆盖 WASAPI 播放，组合门禁以本节十轮为准。
- 编码器重配置三场景 PASS：2859/30→698/15、698/15→698/1、698/1→2859/30，回报均符合请求，127/130/132 帧分别完整解码且无错误；`tools/test-avc-rate.ps1`、`plan-avc-rate-regression-20261121.log` 及 `avc-rate-102457a0e90947e19358d8bdfa10a213/`。较早修复后样本 123 帧也已全部解码，见 `plan-rate-fixed-decode-20261121.log`。未据此宣称画质或实际帧率对照达标，重配置仍可能需要 IDR。
- Flutter analyze 无问题，显示子窗口/录制上传定向测试 6/6 PASS，Windows Release 构建 PASS（124.1 秒）；日志 `plan-final-c-flutter-{analyze,test,build}-20261121.log`。保留已有 native 告警及 SDK `BitrateLimit` 的 C4996 提示。

**C03 整体仍未完成**：标签输入/媒体隔离与合成显示拓扑生命周期已有 loopback 证据；真实音频/物理设备、多屏场景、标签关闭冲突及完整重连/独立第二连接 UI 仍待处理。后续 H03、C04、W3/W4 继续保留；边界见本记录最新结果及[标签页拆分](multi-session-tabs-design-2026-11-21.md)。当前所有修复仍未提交、推送，未执行正式服务安装、防火墙或 ProgramData 写入。

## 修复过程（20:09 起的历史，最终复验见上节）

本节保留当时的定位过程，“进行中”“尚待完成”等为当时状态；音视频组合与解码最终结果以上节为准。

整体任务保持进行中。候选 B 的五轮结果只是该组样本：后续三次基线弱网复测再次出现音频失败，累计空转分别为 2438、2324、2139 ms，设备最大断档 1538、1870、1498 ms；视频帧龄分别为 2072、1565、2464 ms。原日志 `plan-weak-trace-baseline-{1,2}-20261121.log`、`plan-link-trace-1-20261121.log` 保留，不能据前组 5/5 关闭音频问题。

新增可选 `media_timing_trace` 记录每 500 ms 的视频目标码率/FPS、发送字节、丢包、拥塞窗口、RTT、待发视频和音频队列；由测试工具 `--audio-playout-trace` 开启。没有记录媒体内容或凭据。时间轨迹确认拥塞期间视频从 698 kbps 反升至 1061 kbps；音频已连续到达，但旧播放槽过期导致约 1.5 秒实际静音。

当前修复切片：

1. **拥塞控制**：音频预留/编码器舍入使实际视频速率低于名义下限时，后续“降码率”不能反而提高比例。新增回归在修复前失败、修复后通过，并验证链路健康后仍能恢复码率。
2. **音频持续迟到恢复**：在实际已提交音频结束超过 100 ms，且新块播放槽已过期时允许重锚；不再只依赖包到达间隔。仍保留 20 ms 迟到容忍、500 ms PCM 上限及有设备排队/未来播放槽时不触发的边界。只在已启用到达相位修正的播放路径使用新条件。自动化覆盖延迟逐渐增长、频繁到包、恢复突发和恢复后继续播放，`audio_playout`/`video_qos` 2/2 通过；组合实测进行中。
3. **编码器最低码率反升**：进一步区分控制器请求与编码器实际回报。`plan-rate-trace-weak-1-20261121.log` 在 7.38 秒记录请求 698 kbps/15 FPS、实际 947 kbps/15 FPS，质量比例仍为 0.187478。独立 `chast-encode` 探针也复现 2859/30 → 请求 698/15、回报 947/15。Intel AVC 运行时存在按原始图像速率推导的 `BitrateLimit` 下限；2560×1440×12×15/1000/700 取整正是 947 kbps。当前 H.264 已关闭 HRD 信令，在初始化及后续重配置中显式携带 `BitrateLimit=OFF`；保持其它编解码器和产品默认码率不变。修复后同一探针回报 698/15，123 帧编码、0 错误，仍要求 IDR；解码与最终组合复验尚待完成。

上述编码器判断的来源为本地 oneVPL SDK `mfxstructures.h` 的 `BitrateLimit` 说明，以及经指定 SOCKS5 代理读取的 Intel `vpl-gpu-rt` 提交 `bdb5ca28d512cbf02c70b7b7899228577c2e7dd5` 中 `_studio/mfx_lib/encode_hw/h264/src/mfx_h264_enc_common_hw.cpp` 的参数检查。运行时版本可能不同，实际缺陷由本机修复前后探针佐证。原始日志：`plan-encoder-rate-baseline-20261121.log`、`plan-encoder-rate-fixed-20261121.log`。

音频重锚/QoS 修复后的两个弱网单轮样本均通过：先前构建 SHA-256 `3D285CE0B5A7D72AD7E3565D2943B7274366D6F18D74714B70E681C3F8EE138E` 的 `plan-recovery-weak-1-20261121.log` 为 1068 ms 空转、269 ms 最大断档、88 丢弃块、skew +34.5 ms、视频帧龄 1970 ms；加入码率诊断后的 `plan-rate-trace-weak-1-20261121.log` 为 941 ms、144 ms、67 块、+31.6 ms、2340 ms。两个样本都在编码器下限修复之前，不充当最终构建的五轮证据。

原计划次序（2026-11-21 记录）：先完成 C03 音视频组合门禁，再补真实媒体/窗口和显示场景；之后实现的多连接标签页已形成基础实现及本机 loopback 证据。按 2026-11-22 最新状态，先保留标签关闭冲突并补可用物理场景，再推进 H03 上传任务持久化恢复和 C04 输入增强，随后按路线推进 W3 企业协作与 W4 主机/生产功能。不能以 loopback 结果代替物理设备、双机或生产验收。当前机器仍不执行正式服务安装、防火墙或 ProgramData 写入。

## 本轮修复

1. **显示子窗口媒体接口**：原生桥允许同一有效会话的静音、麦克风、语音、录制、设备列表/选择和处理选项；继续禁止子窗口启动、认证或停止整个连接。旧 generation 的媒体调用返回 `session_ended`，只保留原有窗口解绑/输入释放清理例外。新增原生准入测试覆盖允许、拒绝、旧会话和清理路径；现有 Flutter 双窗口状态同步回归继续保留。此前仅有 widget mock 的证据不足，真实设备及窗口操作验收仍单列。
2. **视频参考链恢复**：补画质帧虽不携带新桌面内容，仍可能更新编码参考；发送返回 `WaitTimeout` 后统一标记重同步，由共享订阅请求 IDR 并沿用退避机制，不能把已消费的编码包当作可重试图片。
3. **测试工具退出竞态**：`run_receiver` 的失败返回在复制 `ReceiverStats` 前先停止并 join 音频线程；线程仍保持在所引用资源之后声明，使异常析构也先 join。
4. **Num Lock 映射**：`MAPVK_VSC_TO_VK_EX` 对本机 `E0 45` 返回 0，导致 HID Num Lock 拒绝，触发历史 protocol 失败。直接使用布局无关的 `VK_NUMLOCK`，保留扫描码 `45` 和注入所需扩展标志。新增 US/German 按下/松开回归；没有降低原协议断言。
5. **音频发送和播放时序**：增加独立音频转发工作线程，避免视频/控制循环阻塞采集队列；统计通过互斥快照回收，撤权由传输层重新检查，停止/失败时先 join 再释放资源。播放迟到判断纳入设备现有 padding，恢复后不再额外积压一个设备缓冲；到达相位使用明确的安全余量，避免把启动延迟学习成永久基准。设备统计在 padding 非零时不把采样时差累计成实际断档。

不扩大 500 ms PCM 上限，不放宽 20 ms 迟到容忍，不调整默认视频码率。干净链路继续使用既有 ±40 ms 音画偏移要求；弱网连续性实验预算继续单独列出，不能代替真实听感。

## 验证记录

- 原生：最终候选 B 的 `protocol`、`keyboard`、`display_window_access`、`audio`、`audio_forwarder`、`audio_dsp`、`audio_playout`、`shared_loopback` 8/8 PASS，4.77 秒；日志 `review-candidate-b-unit-20261121.log`。覆盖固定播放参考相位、设备仍有 padding 时不误计断档及实际空队列断档。
- 最终集成：`host_parity`、`desktop_stream`、`viewer_audio_video_isolation`、`quic_weak_network` **4/4 PASS，190.65 秒**，日志 `review-candidate-b-integration-20261121.log`。分别耗时 130.52、23.55、5.52、30.97 秒；覆盖主机权限/会话生命周期、拥塞退避及静态桌面重同步、低 FPS 音视频隔离。弱网 CTest 不启用设备播放，其通过不覆盖下文音视频组合中的帧龄失败。
- Flutter 定向 5/5 PASS；候选 B 原生目标构建 PASS，最终 Flutter Windows Release 构建 PASS（112.5 秒），日志 `review-candidate-b-build-20261121.log`、`review-flutter-candidate-b-build-20261121.log`。保留既有 UNICODE/getenv/LNK4098/LNK4199 告警。
- 音频播放使用合成音源、本机 loopback 与进程内 netem；WASAPI 仅将本测试会话静音，未采集真实麦克风或更改系统默认设备。所有日志保留在 `build/test-temp/review-*-20261121.log`。

测试工具失败返回的修复已通过构建和代码路径检查；本轮没有专门注入提前返回，也没有执行线程检测器。原生准入测试与 Flutter widget 测试不代替真实显示子窗口的设备操作验收。

### 音频候选与失败记录（历史）

修复前干净链路日志 `review-clean-baseline-20261121.log`：主机提交最大间隔 199 ms、采集至提交最大 205 ms，最终 skew +77.8 ms，累计空转 210 ms、丢弃 11 块、设备最大断档 168 ms。发送独立线程解决视频/控制循环拖延音频取包的结构性问题；历史那 3 个主机丢弃缺少分项原始证据，仍不能逐个归因。

候选 A（SHA-256 `3A29ED617D724FA867B1161D68B52D7AF51ADBF7E3D90DD022B10A1DF6F5CC17`）使用更严格的整块播放结束期限。干净 5/5、弱网 4/5 通过；弱网第 1 轮累计空转 2067 ms，超过 2000 ms 预算。日志 `review-final-{clean|weak}-{1..5}-20261121.log` 保留，不能用后续结果覆盖。候选 B 恢复原块尾期限语义，仅把设备 padding 纳入可播放起点，并加入固定到达相位参考；没有放宽既有 20 ms 容忍。

候选 B 的 SHA-256：`058A39AE56C9ADB947C12FCF99D55180167CE6179DACD9176495C8B1431F1821`。固定此二进制交替执行干净/弱网各 5 轮，每轮 25 秒，合成音频静音播放。请求设备缓冲 10 ms（本机实际 22 ms），20 ms 迟到容忍，自适应 jitter、到达相位修正、重锚保留队列；弱网维持 80 ms 延迟、0–20 ms 抖动、3% 丢包、1000 kbps，4–12 秒劣化。音频预算：空转 ≤2000 ms、设备最大断档 ≤1000 ms、丢弃 ≤200 块；干净 skew ±40 ms，弱网 ±120 ms；视频最大帧龄 ≤3000 ms。构建完成后才启动测试，期间没有并行编译或运行其他媒体探针。

| 场景/轮次 | 累计空转 ms | 设备最大断档 ms | 丢弃块数 | 最终 skew ms | 整体退出码 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 干净 1 | 70 | 26 | 3 | +37.5 | 0 |
| 弱网 1 | 1804 | 584 | 156 | +28.1 | **1（视频帧龄超限）** |
| 干净 2 | 0 | 4 | 0 | +9.7 | 0 |
| 弱网 2 | 1558 | 536 | 137 | +25.3 | 0 |
| 干净 3 | 0 | 5 | 0 | +6.3 | 0 |
| 弱网 3 | 1299 | 345 | 109 | +25.6 | 0 |
| 干净 4 | 80 | 27 | 4 | +24.7 | 0 |
| 弱网 4 | 1383 | 368 | 117 | +27.2 | 0 |
| 干净 5 | 10 | 18 | 1 | +25.7 | 0 |
| 弱网 5 | 1490 | 611 | 128 | +30.1 | 0 |

音频预算：干净 **5/5**，弱网 **5/5**；整体验证：干净 **5/5**，弱网 **4/5**，脚本最终退出码 **1**。十轮主机采集/发送丢弃均为 0，音频 CRC/解码/协议检查通过。干净累计空转中位数 10 ms、skew 中位数 +24.7 ms；弱网累计空转中位数 1490 ms、skew 中位数 +27.2 ms。弱网仍有 345–611 ms 的最大设备断档及 109–156 个丢弃块，不能写成无断档或音质达标。完整日志：`build/test-temp/review-candidate-b-{clean|weak}-{1..5}-20261121.log`。

**候选 B 当时未解决：弱网视频帧龄仍有失败。** 五轮分别为 3347、2759、2037、2165、2188 ms；第 1 轮超过既有 3000 ms 门槛。该轮 IDR 序号 102、132608 字节，从提交至接收耗时 3340.3 ms（日志 352 行）；链路恢复后最终帧龄 10 ms。该历史样本定位到这次关键帧交付，但未区分拥塞、重传与队列竞争的各自贡献。后续 QoS/编码器修复和候选 C 同参数组合复验已见上节；本失败证据继续保留。

设备累计断档统计现仅在 padding 为零时计入间隙，避免把 `GetCurrentPadding` 与时钟分开采样的误差重复累计成静音。该统计修正没有改变 `starved`/`dropped` 门槛；新版 `device-starved` 数值不能与旧统计直接作改善幅度比较。

## 仍保留的功能与验收

| 范围 | 尚未完成的功能 | 已有实现但仍待验收 |
| --- | --- | --- |
| 服务器与管理台 | 独立共享地址簿及密码端到端加密/密钥恢复轮换、Control Role、独立 Alarm、用户邀请/邮箱验证/设备白名单、多实例限流与连接路由/存储协调、自有协议 API 回退、定制客户端发布管理 | 生产 PostgreSQL TLS/最小权限/灾备/故障切换、全管理台辅助技术覆盖 |
| 主机/发射端 | 便携特权辅助服务、扩展虚拟显示后端、主机录制源文件保留/清理策略 | H03 服务端 API、Go 持久 worker、C++ 封口队列及真实便携 `chast-host` 整链已通过隔离 loopback；正式 SCM/worker、安全桌面/锁屏/账号切换、Firewall COM 写入与回滚、生产部署/双机、非可信 owner 的 NTFS 集成专项仍待 |
| 查看端/接收端 | C03 标签关闭历史进程退出根因及关闭生命周期一致性 | 无效 HWND 触发的真实 Win32 `PostMessageW` 失败回滚及同进程 3 轮正常关闭/重连已通过 Release loopback；历史 12:05/12:08 进程退出根因仍未定位，且未测试消息队列满载。H03 查看端持久队列及实际 Flutter 上传进程强杀重启已通过本机验收。C04 剪贴板按键发送、触控板速度、边缘滚动/样式、SVG 和快捷键配置均已实现；22:43 Windows Release loopback 覆盖原生 hook 与 Flutter 回退并通过，真实应用间剪贴板互通、键盘/输入法、物理触控板仍待。C03 逐标签媒体/输入隔离、双标签 lease 及合成双显示器窗口生命周期已有 loopback 证据。真实语音/AEC/降噪、设备切换、长时听感、独立第二连接完整关闭/重连、物理混合 DPI/旋转/全屏/热插拔、真实电源行为及 Release 撤权弹窗仍待验收 |

各批测试的数量、版本及范围以上方分别记录为准，不将不同版本测试相加作为全量门禁。未重跑不受影响的 Go/管理台或全量发布矩阵。此处原“未提交/未推送”是该记录形成时的状态；2026-11-23 归档纳入源码、测试与文档，原有生成物仍保留在工作区且不纳入源码提交。

标签关闭冲突已复现，但退出来源未定位；仅补了明确消息投递失败时的模型回滚，历史关闭路径根因仍作为未解决项。H03 查看端恢复和主机本机归档整链均已通过隔离验收；正式部署/双机后置，主机源 MP4 不自动删除，清理策略待定义。C04 按设备条件补物理键盘验收，并补真实应用剪贴板、物理显示、真实音频与电源验收；W3/W4 依既有路线实施。不安装服务、不修改防火墙/ProgramData，不以本机回环证明跨网或正式服务验收。实验预算 PASS 不代表产品音质验收，C03 与 W1 发布门槛继续保持未完成。
