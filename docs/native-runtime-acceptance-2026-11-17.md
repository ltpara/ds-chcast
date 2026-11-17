# W1 / S01–S02 Windows 原生进程联动验收

原记录日期：2026-11-17；补测日期：2026-11-18。原记录分支 `codex/s03-role-console-2026-11-17`、基线 `202a228`；Flutter 补测在 HEAD `f4e5d3b` 的当前工作区执行，未提交、推送或切换分支。

## 1. 结论与范围

本轮完成本机真实 Windows 托盘主机、隧道代理和独立 QUIC 查看端的联动验收，补上 [上一轮浏览器模拟回执](strategy-connection-console-acceptance-2026-11-17.md) 未覆盖的实际进程执行证据。精确断开、另一连接保持、控制面中断、回执上传失败后代理恢复、带连接的主机异常退出、新 runtime 隔离、策略应用及撤回均通过。后续补测又确认受管上限 3 时三个真实会话获准、第四个拒绝；策略撤回后本地上限 2 生效。

2026-11-18 使用当前工作区复跑 `tools/test-native-runtime.ps1`：`TestWindowsNativeRuntimeAcceptance` 通过，耗时 152.82 秒（Go 包含初始化 154.407 秒）。再次确认真实目标 QUIC 会话被精确断开并获得 signed absence receipt，非目标会话保持 active；控制面故障、回执恢复、runtime 隔离及受管 3/4、本地 2/3 会话上限场景通过。该次用例直接调用真实 API，不经过管理台浏览器页面，单独不构成管理台 UI 端到端验收；后续浏览器联测见下一段。产物见下文。

2026-11-18 随后以 `tools/test-native-runtime.ps1 -Browser` 启用真实浏览器联测，`TestWindowsNativeRuntimeAcceptance` 通过，耗时 161.13 秒（Go 包含初始化 161.172 秒）。隔离无头 Chrome 登录真实内嵌管理台，从活动连接列表按 connection ID 打开目标详情、确认并提交断开；页面显示“已确认断开”。测试读取页面真实提交的 request key，再以该 key 重试并确认返回同一 command ID。真实托盘主机/tunnel 执行了断开，目标 QUIC 文件模式会话消失且签名 `confirmed_absent` 回执被接受，另一条 QUIC 会话仍 active。此次将此前“管理台精准断开完全未验收”更新为“真实页面/API/主机单机联测通过”；仍未覆盖 Flutter 视频流中的管理台断开、双物理机或跨网行为。

另运行现有 `chast-viewer-probe` 桌面会话验收：实际 DXGI 主机捕获、硬件 HEVC 编码、Viewer D3D11VA 解码和 QUIC 回环链路均工作，收到 39 个解码帧；文件收发和两个独立终端也通过。这是单机 `ViewerEngine`/原生链路证据，不是 Flutter 窗口验收。

2026-11-18 补充 Flutter Release 回环验收：使用隔离 `APPDATA` 启动真实 `chast_viewer.exe`，通过 `127.0.0.1` 连接仅回环、启用 `--input-dry-run` 的 `chast-quic serve`。主机接受测试密码；查看端显示的完整 SHA-256 指纹与主机日志逐字节一致后，确认首次信任。DXGI 捕获 `1920x1080`，MFX 硬编 HEVC，目标 30 FPS；指纹确认后的会话持续约 266 秒，主机发送 7869 帧，查看端收到并解码 7868 帧，发送错误为 0。实际桌面画面在查看端可见。没有发送键鼠或文件操作；输入计数为 0，主机配置为 dry-run。该结果证明本机完整 Flutter 会话的密码认证、首次证书信任、视频接收与解码路径，不代表双机或跨网验收。

2026-11-18 补充 Flutter 双查看端单机并发验收：在隔离 `APPDATA` 下启动两个独立 Release `chast_viewer.exe`（A PID 6808、B PID 14932），均连接隔离主机 `127.0.0.1:49213`，完成密码认证和首次指纹确认并显示真实桌面流。两端同时在线采样 10 秒，主机/A/B CPU 增量约为 3.27/2.25/1.70 秒。结束 B 后，A 进程仍运行且主机仍存活；将 A 置前后的 2 秒画面样本中，12,040 个采样像素有 11,034 个变化（91.64%）。主机审计确认 B 的会话成功结束：`success=true`，开始时间为本地 2026-11-18 02:15:29.852，结束时间为 02:20:24.661，持续约 294.8 秒。随后本地结束 A，但没有观察到 A 对应的成功会话结束审计；本轮因此证明了同机双 Flutter 查看端并发收流，以及 B 结束后 A 仍持续出图，不证明 A 的正常会话关闭。管理台精准断开由后续独立的文件模式页面/API/主机联测证明，不是本轮并发测试的证据。未执行键鼠或文件操作，非双物理机/跨网测试。证据目录为 `%TEMP%\chast-w1-multiviewer-confirmed-20261118-91e2ee0796264b2a81f30f6937e3f083`，审计文件为 `host.audit.jsonl`；审计中另有早期无密码连接拒绝记录，不计作已认证会话。

2026-11-18 原生键盘回环探针复验：`chast-keyboard-test.exe` 输出 `keyboard failures=0`；`chast-viewer-probe.exe --keyboard` 退出码为 0。该探针使用真实 loopback QUIC、`ViewerEngine` 和主机 `InputInjector`，将输入送入本进程拥有的前台编辑框，并核对 US 布局 `Chast1`、German 布局 `zy/@`（含 Shift/AltGr）、Legacy `VK_Y` 的 `y`、Unicode `直接提交文字` 及 Microsoft Pinyin `你好`。因此验证的是单机 Windows 原生键盘注入和文本结果，不是 Flutter 窗口、双机或管理台断开验收。

2026-11-18 尝试以 `tools/test-native-runtime.ps1 -Browser -Flutter` 将真实管理台断开扩展到 Flutter 视频会话，四次运行均未完成该场景：首次自动化未取得 Flutter 主窗口；修正窗口可见性/聚焦后，三次运行均未在主机活动连接快照中观察到 Flutter 会话，最终超时退出。管理台浏览器断开步骤因此没有执行。这是尚未通过的 opt-in 测试夹具，不是 Flutter 会话或管理台断开验收证据；底层原因仍未定位。最后一次日志位于 `build/native-runtime-20261118-061235-0bcc1b79/run-2153063024/`，此前运行目录为 `build/native-runtime-20261118-055953-d1b7c3d1/`、`build/native-runtime-20261118-060437-c70527d7/` 和 `build/native-runtime-20261118-060957-4b0d39bc/`。

没有发现需要修改生产逻辑的新缺陷。本轮新增的是测试工具、显式启用的集成用例、构建目标和执行脚本；不修改协议或 schema（仍为 v26）。

验收边界：

- `chast-host.exe` 为实际托盘程序，以隔离 `--portable --tray --config` 启动，实际消费策略、发布连接快照并执行断开；没有使用 FakeHost。
- `chast-tunnel.exe` 为实际 Go 程序，初始由托盘启动，故障后另行启动同一程序，签名上传生产协议快照/回执并持久化命令。
- 原生进程联动验收中的每个查看端为独立 `chast-live-viewer-probe.exe` 进程，使用生产 `QuicSession`、固定证书指纹和实际握手。该项连接是文件模式，不采集屏幕、不传文件、不注入输入，也没有启动 Flutter UI；Flutter Release 回环补测单独记录于下文。
- 管理 API 通过 `testutil` 启动真实处理器、SQLite、身份/注册与中继组件；HTTP 故障由仅回环的测试代理注入。未启动独立 `chast-server.exe` 或生产数据库。
- 受管连接上限场景配置隔离的访问票据签名密钥，探针为每次连接申请新票据并提交持有证明；撤回策略后的本地上限场景不带受管票据。
- 所有程序运行在同一台 Windows，网络为 loopback；未安装 Windows 服务、修改已有主机配置或防火墙。测试设备/密钥均为虚构数据，LAN 发现关闭。

这些是 Windows 单机验收，不能扩大为双物理机、跨 NAT、SCM 服务、完整 Flutter 功能矩阵验收或完整发布验收。

## 2. 实际执行结果

| 场景 | 操作与判据 | 结果 |
| --- | --- | --- |
| 实际主机注册 | 启动托盘，等待其实际 tunnel 注册设备与指纹，并产生原生快照 | 通过 |
| 多查看端 | 分别启动 A/B 两个进程，完成固定指纹的 QUIC 握手，API 收到两条不同连接 | 通过 |
| 精确断开 | HTTP 创建 A 的指令，等待 A 的实际传输断开、原生连接消失及服务端接受 applied + confirmed_absent | 通过；B 的传输和 API 活动态保留 |
| 同一次请求重试 | A 结束后以相同 request key 再发请求 | 返回同一个 command ID |
| 控制面暂时不可用 | 代理返回 503，再创建 B 的断开请求；保持 6 秒并确认真实 B 仍连接 | 指令保持 queued，不误报成功 |
| 原生已执行但回执上传失败 | 恢复其他 API，只阻断 receipts；B 实际断开，原生文件留下明确回执 | 服务端保持 delivered、未确认；连接消失不替代签名回执 |
| agent 异常退出并恢复 | 只终止该托盘的 tunnel 子进程，重启同一实际二进制并复用原文件 | 恢复上传已落盘回执，原 command ID 变为 applied + confirmed_absent |
| 带活动连接的主机异常退出 | 再连一个查看端，强制终止测试托盘；查看端从传输检测断开 | 通过；探针 idle timeout 为 15 秒，生产查看端超时未改 |
| 主机重启 | 用同一个隔离配置启动实际托盘，重新连接查看端 | runtime ID 更新，新连接正常到达 API |
| 非空旧 runtime 指令 | 暂停控制面，注入旧 runtime 的非空命令文件，并特意指向当前新连接、使用未来截止时间 | 新连接仍在，原生没有产生该命令回执；不是空队列测试 |
| 策略应用 | 向 nativebox 分配 `host.max_viewers=3` locked 策略 | 实际监听器重启，runtime 更新，当前查看端结束；服务端收到对应当前 revision/digest 的真实 applied 回执 |
| 受管连接上限 | 策略应用后使用访问票据依次建立 3 条 QUIC 文件会话，再尝试第 4 条 | 3 条完成握手并出现在服务端活动列表；第 4 条未建立会话 |
| 策略撤回与本地上限 | 正常结束三个受管探针，撤回分配并等待当前版本回执；随后建立 2 条本地会话并尝试第 3 条 | applied；空覆盖恢复，本地配置仍为 `max_viewers=2`；2 条获准，第 3 条拒绝，未把受管值写成本地偏好 |
| 本机桌面流 | `chast-viewer-probe` 启动真实主机和 ViewerEngine，经 loopback 建立桌面会话 | DXGI 捕获 `1920x1080`，Intel MFX 硬编 HEVC，Viewer 使用 D3D11VA 并收到 39 帧；文件操作和两个终端窗口通过 |
| Flutter Release 视频回环 | 隔离 `APPDATA` 启动真实查看端，密码认证后核对并确认首次主机指纹，连接仅回环、`--input-dry-run` 主机 | 会话持续约 266 秒；主机发送 7869 帧，查看端收到/解码 7868 帧，目标 30 FPS、发送错误 0；主机输入计数 0 |
| 原生键盘回环探针 | `chast-viewer-probe.exe --keyboard`，使用真实 loopback QUIC/ViewerEngine 并注入到受控编辑框 | US、German `zy/@`/AltGr、Legacy `VK_Y`、Unicode 中文及 Microsoft Pinyin `你好` 文本断言通过；退出码 0；键盘单测 `keyboard failures=0` |
| 两个 Flutter Release 查看端并发 | 两个独立隔离配置均完成密码和指纹确认；同时收流，结束 B 后观察 A 画面变化 | 并发采样约 10 秒；主机/A/B CPU 增量约 3.27/2.25/1.70 秒；B 有 `success=true` 结束审计（约 294.8 秒），B 结束后 A 仍有 91.64% 采样像素变化；A 本地结束后没有对应成功结束审计，故不作为 A 正常关闭或管理台精准断开的证据 |
| 管理台页面精准断开 | 隔离无头 Chrome 登录嵌入式管理台，按目标 connection ID 打开详情并在页面确认断开；由页面真实发起 API 请求 | 页面显示“已确认断开”；目标签名缺席回执被接受，目标文件模式 QUIC 会话消失，另一会话仍 active；页面 request key 重试返回同一 command ID；真实浏览器/页面/API/主机单机联测通过 |

三/四及二/三连接边界由实际独立进程和 QUIC 握手验证，不只是单元测试。管理台真实页面已对文件模式 QUIC 会话完成精准断开，确认目标签名回执、非目标会话保持及 request key 幂等重试。Flutter 回环补测验证了密码认证、首次指纹确认后的正常会话和持续视频接收/解码。另有两个 Flutter 查看端同时收流、B 结束后 A 继续出图的单机证据；A 的本地结束没有成功结束审计，因此不作为 A 正常关闭证据。Flutter 窗口键鼠/文件操作体验、Flutter 视频会话中的管理台精准断开和双机行为仍待验。原生键盘回环探针已验证 Windows 布局映射、AltGr、Unicode 及主机拼音文本输入。策略变更会重启监听器并结束现有连接；撤回前先正常结束三个受管探针。

桌面流探针与受管连接上限测试是两项不同验收。桌面流探针的 `FrameSink` 只记录真实解码帧数；截图 API 使用确定性合成像素，因此不把探针 PNG 作为真实画面内容证据。桌面流与连接上限两项原生探针没有启动 Flutter UI，也没有注入键鼠。独立原生键盘探针使用真实输入注入，但目标是本进程创建的受控编辑框。Flutter 补测使用真实 Release 应用并完成首次指纹确认；未发送输入事件，dry-run 配置确保即使收到输入，也不会注入操作系统。

旧 runtime 场景是主动构造本地缓存反例：保留旧 runtime、使用新连接 ID 和未来 deadline，使运行实例校验成为必要条件。它不代表通过了远程攻击者伪造服务器签名；签名与授权校验另有现有 API/agent 回归。

## 3. 运行证据

环境：Windows `10.0.26200.0`、Go `go1.27.4 windows/amd64`、CMake Release 原生构建。完整最终执行目录：

2026-11-18 管理台浏览器联测目录：`build/native-runtime-20261118-051723-6d6be834/`，运行目录：`run-1261003824/`。

- `acceptance.log`：真实页面记录 `browser UI confirmed target disconnect 35355a54a17bcd169a80881df0f0f856`；随后目标签名缺席回执被接受，非目标 `98016280833d8bd84916f1915993fa86` 保持活动；最终 `TestWindowsNativeRuntimeAcceptance` **通过，161.13 秒**（Go 包含初始化 161.172 秒）。
- 本运行目录含主机、代理、两个查看端及连接上限探针日志；管理台浏览器为隔离无头 Chrome 临时配置，关闭后清理。

2026-11-18 当前工作区复跑目录：`build/native-runtime-20261118-044608-f9561864/`，运行目录：`run-83640320/`。

- `build.log`：本次复建主机、tunnel 依赖和原生查看端探针通过。
- `acceptance.log`：`TestWindowsNativeRuntimeAcceptance` **通过，152.82 秒**；Go 包含初始化共 154.407 秒。实际目标断开、非目标保持、故障恢复、runtime 隔离及受管/本地会话上限均通过；测试经真实 API 直接发起命令，不覆盖管理台页面。
- 本轮另复跑 `server` 目录的 `go test ./...` 与 `go vet ./...`，均通过；管理台 `node --test internal/webadmin/app_session_test.cjs` 为 47/47 通过。Flutter 全量测试 814/814、`flutter analyze` 通过，键盘探针结果见上文。

原综合验收详细目录（2026-11-17）：`build/native-runtime-20261117-215546-103b969b/`，运行目录：`run-3792090675/`。

- `build.log`：重建 `chast-host`、依赖 `chast-tunnel` 和 `chast-live-viewer-probe` 成功；保留既有运行库冲突及未使用延迟导入链接警告。
- `acceptance.log`：`TestWindowsNativeRuntimeAcceptance` **通过，151.68 秒**；Go 包含初始化共 153.516 秒。该结果是一项跨进程综合用例，不是独立功能测试的数量汇总。
- `run-3792090675/`：探针连接/拒绝日志、原生当前快照/命令文件、策略缓存/回执，以及注入的 `replayed-old-runtime.json`；包含 `managed-cap-1..3`、`managed-cap-fourth` 和 `local-cap-1..2`、`local-cap-third` 的独立进程记录。
- `%TEMP%\chast-w1-cert-20261118-f32caa63185c4ac785d419455ce08090\host-final.stdout.log`：Flutter Release 回环主机日志，记录密码准入、首次指纹确认后的 DXGI/MFX 会话、7869/7868 帧统计和 0 个输入事件。测试令牌已随主机退出失效。

本轮 `flutter analyze` 通过，`flutter test` **814 项通过**；`flutter build windows --release` 成功，产物为 `app/build/windows/x64/runner/Release/chast_viewer.exe`，耗时约 353 秒。构建保留现有 MSVC `LIBCMT`/`d3dcompiler_47.dll` 链接警告及 Unicode 宏重定义警告。CMake Release 原生目标重建成功；新增 Go 验收用例可编译，并由上述原生进程验收实际运行。

后续无副作用的服务构建检查：`cmake --build build --config Release --target chast-service -j 4` 成功，`.\build\Release\chast-service.exe --help` 退出码为 0。构建保留 `LIBCMT` 默认库冲突警告。未执行 `--install`、`--run` 或 `--worker`，因此不构成 SCM 生命周期、服务身份或 worker 桌面验收。

随后补充的 Flutter 策略定向验收：`flutter test test/session_viewer_strategy_test.dart` **5/5 通过**，全量 `flutter test` **814/814 通过**，`flutter analyze` 无问题。页面级用例验证未配置项采用 default 建议；续票取得新 revision 不改变当前会话的五项设置；下一会话应用新建议；持久化显式本地 tunnel mode、WebSocket、录制归档、初始剪贴板和 view fit 偏好覆盖 default。策略 delivery、tunnel 和 native session 均由测试注入；这不是服务器策略撤回、双机或完整 Flutter 实机验收。

桌面流补充命令：

```powershell
cmake --build build --config Release --target chast-viewer-probe -j 4
ctest --test-dir build -C Release -R "^viewer_folder_download$" --output-on-failure
& .\build\Release\chast-viewer-probe.exe
```

Release 构建成功；CTest `viewer_folder_download` 通过（4.74 秒），直接运行退出码为 0，输出确认 DXGI、MFX HEVC、D3D11VA、`1920x1080` 和 39 个解码帧。

补充验证（`server` 目录）已运行：

```powershell
go test ./internal/api -run '^$' -count=1
```

通过。`go vet ./internal/api` 通过。依照用户要求，本轮未查看 Git 状态或差异，也未运行 `git diff --check`。

日志中的 `proxy error: context canceled` 出现在故意终止/重启代理的阶段，测试没有失败。结束后确认本轮 host/tunnel/probe 进程均为 0；临时服务由测试清理。测试产物留在 build 目录。没有重跑上一轮 Node 45 项、Go 全量、PostgreSQL 或原生全量 CTest；Flutter 静态分析和 814 项测试已在本轮执行并单独记录。

## 4. 后续复跑

已配置好 CMake 构建树的 Windows 开发机执行：

```powershell
.\tools\test-native-runtime.ps1
# 启用隔离无头 Chrome 的真实管理台页面断开步骤：
.\tools\test-native-runtime.ps1 -Browser
# 未验收的 Flutter 视频会话 UI 步骤；当前会超时，不作为通过证据：
.\tools\test-native-runtime.ps1 -Browser -Flutter
# 使用其他已有构建树：
.\tools\test-native-runtime.ps1 -BuildDirectory D:\path\to\build
```

脚本构建当前代码，每次创建独立产物目录，运行后恢复原有环境变量。用例在未设置 `CHAST_NATIVE_BIN_DIR` 时明确跳过，默认 `go test` 不会启动桌面主机。`CHAST_NATIVE_ARTIFACT_DIR` 用于保留运行目录；未设置则使用自动清理的测试目录。二进制清单见脚本。清理只针对测试启动并持有的 PID 和子进程树。

## 5. W1 后续工作

1. 第二台可连接 Windows 测试机上运行完整 Flutter 查看端，与本机主机组成跨机拓扑；在两个真实 Flutter 视频会话中执行管理台精准断开，确认非目标会话持续收图并验证实际键鼠/文件操作。管理台页面对真实主机和两个 QUIC 文件模式查看进程的单机精准断开已通过，含签名缺席回执、非目标保持活动及幂等重试；单机 Flutter Release 已完成首次证书确认并持续约 266 秒收到/解码 7868 帧，双查看端并发和 B 结束后 A 持续出图已在单机验证，原生键盘探针也已通过。Flutter 窗口键鼠/文件操作、Flutter 视频会话中的管理台精准断开及跨机行为仍待验。
2. 自动化层的 host/viewer default、locked、显式偏好、未知键及下一会话快照已有对应回归；本轮补充 `SessionPage` default 合并与续票快照固定测试。仍需真实查看端验证逐键回执，以及服务器策略变更/撤回后下一次连接采用新 revision。受管三连接准入边界已由本机进程验收覆盖。
3. 在明确的服务验收环境验证 SCM 服务/worker 启停、账号切换和安全桌面；本轮便携托盘不能替代服务路径。
4. 验证真实链路断网、重连、跨 NAT/代理、旧回执重放及较长故障窗口；本轮 503 故障只覆盖 HTTP 控制面。
5. 最后汇总 W1 缺口，执行发布所需完整门禁；生产数据库、灾备及性能对照继续按既定后续阶段安排。

当前环境没有已连接的第二台 Windows 验收主机，跨机项仍标待验收；不提前宣称 W1 发布完成或转入 C01/P2。
