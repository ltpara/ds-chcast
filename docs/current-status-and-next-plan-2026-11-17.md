# 当前完成状态与后续计划

日期：2026-11-26（2026-11-18 状态基线延续）

分支：`codex/complete-workspace-2026-11-20`

2026-11-26 CTest 可重复安全门禁：CMake 对经副作用审查的 69 项加 `reviewed_non_destructive` label；allowlist fail-closed，新注册测试在人工审查前不会进入该集合。CTest dry-run 确认该 label 命中 69 项、补集 36 项。Release 顺序执行 `ctest --test-dir build/root -C Release -L '^reviewed_non_destructive$' --output-on-failure --parallel 1`，结果 **69 项：65 PASS、4 SKIP、0 FAIL**，234.10 秒。4 个 skip 为 `clipboard_listener`、`clipboard_listener_startup`、`machine_state_security_untrusted_owner`、`viewer_clipboard_native`，均由既有权限/Window Station 环境限制触发；未计为 PASS。完整原始日志：`build/test-temp/isolated-ctest/reviewed-nondestructive-20261126-LastTest.log`。这只是审计过的安全子集，剩余 36 项仍需隔离 Windows 桌面/设备环境，不等于全量 105 项门禁。

2026-11-25 本机自动化门禁复跑：Flutter `test --no-pub` **881/881 PASS**、`analyze --no-pub` 无问题；`server` 全量 `go test ./...` 与 `go vet ./...` PASS；管理台 Node `app_session_test.cjs` **53/53 PASS**，浏览器断开脚本 `node --check` PASS；CMake Windows Release 全目标构建 `cmake --build build/root --config Release --parallel 4` PASS；定向 CTest **10/10 PASS**（host settings/strategy、Firewall 规则与 fake transaction、SCM install/uninstall fake transaction/stop、host parity、锁屏策略与 tunnel 参数）。`host_parity` 本地回环运行 124.84 秒。测试没有安装/卸载真实服务、写防火墙或 ProgramData，也不是全量 105 项 CTest、部署浏览器/屏幕阅读器、双机、生产 PostgreSQL/SMTP、外部设备或复杂网络验收；这些门禁继续待验收。

2026-11-25 非破坏性 CTest 补充筛选：新增 **22/22 PASS**。纯逻辑/协议组 7 项（cursor、remote_cursor、annotation_paint、protocol、session_tab_model、keyboard、protocol_fuzz）；隔离临时目录、合成媒体或本机回环组 15 项（recording_upload_queue、service_log、platform_process、file_snapshot、rollback_stack、desktop_stream、recording_disk、host_voice、display_layout、host_i18n、audio、audio_forwarder、audio_dsp、audio_playout、carrier）。CTest 为每项配置 `build/root/test-env/<test>` 独立 TEMP/TMP；host_voice 使用合成麦克风/播放器及本机回环，desktop_stream 使用测试替身。第一次调用未指定多配置构建所需的 `-C Release`，因此仅报告 Not Run；随后带 `-C Release` 重跑的两组测试均通过。此筛选不是 105 项完整 CTest，也未涉及真实服务/防火墙/ProgramData、当前桌面捕获、摄像头、显示拓扑或用户 Downloads；这些项目继续逐项审核副作用后再决定是否运行。

2026-11-25 非破坏性 CTest 第二轮：再运行 **12/12 PASS**（video_qos、decoder_padding、decode_rejects_vp9、quic_microphone_reference、quic_weak_link_budget、access_ticket、host_live_connection、shared_loopback、file_transfer、mp4_recorder、host_security、host_tunnel）。这些用例分别为纯逻辑/协议、合成音频或只在构建/CTest 临时目录写入文件；host_tunnel 使用测试子进程而非真实隧道部署。`host_tunnel` 已包含在此前 10 项定向门禁中，因此本轮增加 11 个不同的 CTest 覆盖。未运行读取当前桌面的宿主/捕获测试、真实摄像头/麦克风、改动剪贴板/显示拓扑、向用户 Downloads 写文件或触碰 SCM/防火墙/ProgramData 的用例；全量 CTest 仍未执行。

2026-11-25 非破坏性 CTest 第三轮：10 项中 **9 PASS、1 SKIP**（cursor_hint、display_window_access、display_subscriptions、display_video、viewer_display_streams、machine_state_security、machine_state_security_untrusted_owner、display_privacy、managed_access、host_ticket_lease）。显示隐私用例使用 fake backend，不调用 `SetDisplayConfig`；多显示器协议用例使用假 decoder/本机 loopback；managed access 与 ticket lease 均为无视频的 loopback 会话。`machine_state_security_untrusted_owner` 仅在 CTest 临时目录构造 ACL fixture；因当前 token 未分配 `SeRestorePrivilege`（错误 1300）按设计 SKIP，真实 owner 迁移集成路径仍待有权限的隔离环境验收。

2026-11-25 clipboard CTest 副作用核对：`clipboard_oledata` **PASS**（进程内 IDataObject，不交接系统剪贴板）；`clipboard_listener`、`clipboard_listener_startup` 与 `viewer_clipboard_native` **SKIP**。前者和 native probe 明确因创建私有 Window Station 返回访问拒绝（错误 5）；startup probe 在私有 station 初始化阶段返回 CTest 配置的 skip code。它们均未在操作者剪贴板执行写入。后续依据实际副作用逐项判断，不以被跳过的用例记作通过。

2026-11-25 补充纯合成与文件会话用例：`capture_transform`、`png_writer`、`viewer_files_only`、`viewer_files_only_denied` **4/4 PASS**。前两项使用合成纹理/WIC 并将产物限制在构建测试目录；后两项通过本机 loopback 验证文件专用会话，fixture、下载和上传均位于 `build/root/test-temp`，会话断言为 0 显示器、0 帧，不读取宿主桌面或用户 Downloads。另运行 `desktop` **1/1 PASS**：只读确认当前 Default 输入桌面，并把 DPI awareness 设在该测试子进程内；未切换桌面或改变系统显示设置。

2026-11-25 `viewer_clipboard_native` 私有站探针复核：CTest 按设计标记 **SKIP**；日志明确为创建私有剪贴板 Window Station 被当前进程 token 拒绝（错误 5），因此未执行剪贴板读写。此项与两项 listener 的 skip 属同一环境策略限制，不能计为功能通过。

2026-11-25 纯本机/fake-fixture CTest 补充：`reference_counted_lease`、`portable_helper_lifecycle`、`virtual_display` **3/3 PASS**。helper lifecycle 只用假 SCM backend，并在 `%TEMP%` 创建/清理测试映像；virtual display 使用 fake 安装流程、临时 ProgramData 覆盖及只读 `status` 探针，未安装驱动、触碰真实 `%ProgramData%` 或改变显示拓扑。

2026-11-25 C01 电源租约补充：`viewer_awake_request` **1/1 PASS**。默认测试即时 acquire/release 两个进程级 lease，并验证最后一次 release 清除 Windows awake request；未使用延迟保持参数或改动持久电源计划。

2026-11-25 标签窗口可见性 CTest 补充：`session_tab_visibility` **1/1 PASS**。用例创建并短暂显示两个 100×100 测试窗口，通过 `SW_SHOWNOACTIVATE` 切换可见性，不请求焦点，结束前销毁窗口；属于短暂 UI 可见性验证，不代表实机多显示器验收。

2026-11-25 `security` CTest **1/1 PASS**（0.64 秒；日志 `build/test-temp/isolated-ctest/security-20261125.log`）。CurrentUser DPAPI 仅对合成字符串/entropy 做往返；device-key 与损坏文件夹具位于 CTest 专属 TEMP/TMP `build/root/test-env/security`，测试后无残留。DPAPI Protect 目录运行前已存在，运行前后条目名相同；测试未访问应用配置或持久设备身份文件。

2026-11-25 `window_focus` 隔离化：测试现在创建唯一的私有 Windows Desktop，在该 desktop 上创建/聚焦临时窗口，绝不调用 `SwitchDesktop`；销毁窗口后恢复原 desktop 并关闭私有 desktop。不可创建/附着时以 CTest skip code 77 报告。Release 目标构建通过，CTest 使用 `--repeat until-fail:3` **3/3 PASS**（0.36 秒）；日志 `build/test-temp/isolated-ctest/window-focus-repeat-20261125.log`。此项现在不再需要用户桌面抢焦点。

本轮筛选累计涉及 **69/105 个不同 CTest 条目：65 PASS、4 SKIP、36 未运行**（前序重复执行的 `host_tunnel` 按不同条目去重）。4 个 skip 是 `machine_state_security_untrusted_owner`（当前 token 缺少 `SeRestorePrivilege`）及三项私有 Window Station clipboard 测试（当前策略拒绝创建/访问）。余下未运行项中仍有真实桌面捕获、摄像头/音频设备、OS 输入/电源或副作用尚未厘清的场景；不会将部分筛选表述为完整 CTest 门禁。

2026-11-25 对剩余 36 项做源码/CTest 命令副作用盘点（本次未运行）：其中 **36 项依赖真实桌面会话或宿主捕获流**。Host/viewer 组为 `shared_capture`、`shared_capturer`、`host_multi`、`host_control`、`clipboard_async`、`host_login`、`block_input`、`multi_session`、`viewer_session_isolation`、`viewer_recording_revocation`、`viewer_audio_video_isolation`、`viewer_clipboard_files`、`viewer_clipboard_files_up`、`viewer_file_conflict`、`viewer_directory_upload`、`viewer_folder_download`；其 HostServer 配置未替换默认桌面捕获器。另有 20 项 `chast-quic verify` 场景：`quic_loopback`、`quic_port_forward`、`quic_port_forward_denied`、`quic_port_forward_local`、`quic_terminal`、`quic_terminal_denied`、`quic_camera`、`quic_camera_denied`、`quic_stream_options`、`quic_idle_disconnect`、`quic_file_browse`、`quic_file_compression`、`quic_loopback_ffmpeg`、`quic_stream_options_ffmpeg`、`quic_merged`、`quic_carrier_tcp`、`quic_carrier_ws`、`quic_recording`、`quic_recording_hevc`、`quic_weak_network`；`run_sender` 未注入捕获替身，采用宿主默认捕获路径。其中特定用例还会启动 ConPTY、可能打开可用摄像头、执行 privacy 场景或录制文件。`viewer_folder_download` 的测试端实际把接收目录指定为构建目录下的 `test-temp/chast-viewer-probe-<pid-tick>/downloads` 并在结束时清理；CTest 注释中“用户 Downloads”已过期。该用例仍未运行，因为 HostServer 未替换默认桌面捕获器，且测试会启动 ConPTY 并验证捕获的桌面截图。

2026-11-25 隔离环境盘点：当前执行环境未发现可用的 Windows Sandbox、VMware/VirtualBox 管理 CLI、Docker CLI 或 Hyper-V `Get-VM` 命令；虽有 WSL，但它不是隔离的 Windows 桌面，不能作为这些 Win32 CTest 的安全替代环境。因此这 36 项继续不在当前交互式主机运行。下一步需先准备一次性 Windows 测试 VM/账户，再在其中逐项运行并保留真实桌面/设备/用户 profile 测试的独立结果；4 项受 token/Window Station 策略限制的 skip 仍须单独报告，不折算为通过。当前登记的 105 项仅为 `ctest -N` 清单数，不代表全量运行或全量通过。

2026-11-25 C03 tooltip 回归根因修复：主消息循环每次 dispatch 后都会调用 `PruneClosed()`，而该函数即使没有已关闭子窗口也无条件重建标签并隐藏 tooltip。诊断 trace 在没有 `prune-child` 的情况下记录到 6,699 次无效标签重建，且 tooltip show 后立即 hide；证据 `build/native-runtime-20261125-213212-645ec9cd/run-1492538567/session-tabs.tsv`。现仅在实际移除子窗口时重建标签并同步可见性。先完成 3/3 轮定向 loopback，随后固定 Windows Release 构建再做 20/20 轮关闭/重连；初次及每轮重连的长标题 tooltip 文本和移出隐藏断言通过，兄弟连接全程 active，末尾精确断开通过。20 轮轨迹记录 20 个 request/post/prune 和 142 条 runner 事件，未见消息泵 quit/error、宿主销毁或窗口集归零；验收 `build/native-runtime-20261125-214349-19e1a47e/acceptance.log`，耗时 220.99 秒。该修复解释并消除本次 tooltip 空文本回归；11-22 历史进程退出根因仍未定位，不能据此宣布关闭生命周期整体收口。

2026-11-25 C03 失败回滚回归复验：在同一 Release 多标签 loopback 中，将一次真实 `PostMessageW` 目标替换为无效 HWND，观察到 `ERROR_INVALID_WINDOW_HANDLE (1400)` 后恢复标签数/选中项/可见窗口且进程仍存活；其后 3/3 轮关闭/重连的 tooltip、兄弟连接及末尾精确断开均通过。证据 `build/native-runtime-20261125-214844-e6397beb/acceptance.log`，50.87 秒。该注入只覆盖无效 HWND，不模拟消息队列满载，也没有复现 11-22 的进程退出。

2026-11-25 C03 历史退出诊断能力复核：现有测试构建可记录 runner 窗口集、标签宿主与消息循环终止事件；Go 验收器在关闭 UI 验收失败时最多等待 2 秒并附查看端退出码（十进制/十六进制）及 `Wait` 错误。`TestNativeProcessExitStatusIncludesCode` 本轮重跑通过（退出码 37 格式断言）。这只证明未来故障的诊断路径可用，不回溯解释 11-22 退出，也未证明会自动收集 crash dump 或 Windows/WER 事件；历史根因仍未定位，关闭路径不做推测性修改。

2026-11-25 C03 WM_QUIT 诊断补强：runner 在测试 trace 开启时会于即将执行 `PostQuitMessage` 前记录 `window-post-quit`，用于把发起 WM_QUIT 的窗口与主循环退出轨迹关联。当前 Release loopback 3/3 轮关闭/重连通过，3 组 request/post/prune 均完整且没有该事件或 runner 终止事件；这不是历史故障复现，不能据此关闭 11-22 未定位问题。详见[多连接标签页记录](multi-session-tabs-design-2026-11-21.md)。

2026-11-25 W1/C01 本机配置语义定向复验：Flutter `view_prefs_test`、`session_start_polling_test`、`lan_discovery_test`、`session_viewer_strategy_test`、`viewer_strategy_test` 合计 **43/43 PASS**；host `host_settings`/`host_client_strategy`/`host_tunnel` CTest **3/3 PASS**；Go `server/internal/strategy` 与 `server/internal/lan` 均 PASS，其中 LAN 包测试实际用回环 UDP 验证 responder/discover 往返及过滤/合并行为。覆盖按设备隔离的默认仅查看偏好、查看端会话保活租约获取/释放、LAN 发现结果合并、host 本地设置/显式偏好与 tunnel 参数传递、服务端 Strategy 覆盖/回执，以及 viewer Strategy locked/default 优先级与会话快照语义。仅为本机定向自动化与回环证据；不代表真实物理局域网扫描、Windows 电源行为或双机验收。

2026-11-25 21:51 只读设备盘点：当前仅有一个活动逻辑显示器 `2560×1440`；PnP 另列多个状态为 Unknown 的显示器节点。音频端点中 ToDesk 虚拟设备与显示器音频为 OK，Realtek USB 麦克风/耳机端点为 Unknown，无法据此确认独立物理设备可用。未改变显示拓扑、打开/采集麦克风或修改系统设置；混合 DPI/多屏及真实音频验收继续待设备确认。

账号安全代码提交前基线为 `dc63487`；本分支依次包含 `34e5a9b`、`4d3cd2b`，C03 tooltip 修复代码提交为 `3bd1214` 并已推送至远端。`688638a` 和 `172516e` 为更早开发基线。本页下方按日期保留的历史状态仍按当时证据理解。

本文作为当前状态入口；2026-11-21/22 审核后的源码修复、验证结果及剩余问题见 [分支审核问题修复](review-fixes-2026-11-21.md)，阶段明细见 [W1 状态与后续计划](w1-status-and-next-plan-2026-11-19.md)。下文按日期保留较早证据，不把历史音频样本或 widget 回归等同于当前原生桥及播放验收。

2026-11-25 C03 关闭/重连复验：当前 Windows Release 构建通过单机 Flutter loopback **20/20** 个选中标签关闭/重连周期；每轮兄弟连接保持 active，末尾管理台精确断开也通过。轨迹 `build/native-runtime-20261125-094542-a80c0caa/run-1612339926/session-tabs.tsv` 记录 20 个 `close-tab-request`、20 个 `close-tab-post`、20 个 `prune-child` 和 142 条 runner 生命周期事件；无 `runner-window-set-empty`、`runner-empty-tabs`、宿主销毁或消息泵退出/错误事件。此次没有复现 11-22 历史进程退出；测试未进入 `PostMessage(WM_CLOSE)` 失败分支，因此 C03 根因仍未定位，不将 20 轮通过作为缺陷已修复的依据。既有 49 轮后续通过记录之外，本轮另增加 20 轮通过。验收日志同目录 `acceptance.log`，总耗时 221.13 秒；构建日志 `build.log`、`flutter-build.log`。

2026-11-25 20:55 融合后 C03 tooltip 回归未通过：Release loopback 初始长标题及第一轮重连通过，第二轮重连后 `#4` 标签提示返回空文本，窗口下仍为 tab-control；用例退出 1。证据 `build/native-runtime-20261125-205510-fa1339bf/run-2331120209/`。本轮包含同步重绘改动；提示窗口在重连路径的生命周期/显示时序仍待定位，未降低断言。该问题与 11-22 历史查看端进程退出根因分别跟踪。

2026-11-24 C03 标签体验与回归：长标签悬停提示的自动化文本/显隐断言通过；同名标签按经校验的 Host ID 短尾号或稳定会话序号消歧，跨进程 caption 唯一断言通过。对应 Flutter Release loopback 分别通过；Flutter 全量 871/871、analyze 无问题。专项截图捕获不可靠（画面显示底部工具栏而非提示文本）；随后增加的同步重绘改动尚未回归，因此 tooltip 视觉证据仍待补。详见[多会话标签设计](multi-session-tabs-design-2026-11-21.md)。这些单机证据不替代双机、屏幕阅读器或物理显示验收。

2026-11-24 验收环境：仅枚举到一台 `IDV10bit2K144` 显示设备（是否物理显示器未确认），未枚举到独立物理麦克风；物理多屏/热插拔和真实麦克风/AEC 场景仍待实机验收，未改变显示拓扑或打开麦克风。

2026-11-25 H01 IPC 与生命周期首片：源码复核确认已有本机 UAC 一次性主机接管和固定名 SYSTEM SCM 服务；缺口是与正式服务隔离、会话期间复用的受控特权桌面辅助服务。新增独立版本化本地 IPC framing：固定 header、随机 128-bit helper instance ID、单向连续序号、request ID、1 MiB 负载上限；错误版本/类型/flags、畸形长度、尾随数据、超限负载、重放、跳号和跨实例均拒绝。Named Pipe 使用 SYSTEM/管理员 ACL 和调用方从已授权交互用户 token 显式传入的 logon SID，并拒绝远程客户端；管道双方核对预期 PID、映像、用户 SID、logon SID、Windows session 和进程创建时间。Release `chast-host-parity-test --elevation` 重建后通过同用户隔离子进程双向帧收发、logon SID 不匹配拒绝、accept/receive 超时清理、断连封闭、非法授权 SID 拒绝及本地拒绝帧不消耗序号回归，旧 UAC 接管/撤权取消/身份回滚/DPAPI-TOTP 连续性也通过。生命周期协调器及 Win32 SCM adapter 代码已加入；服务名和 ProgramData 暂存映像路径按 instance ID 限定，校验 SCM 服务类型/启动方式/账户/映像/控制 DACL，并向服务传递父进程身份。fake 后端回归覆盖正常启停、冲突隔离及失败回滚，但**未调用 Win32 adapter 或真实 SCM**。专用 helper service entry、staging/copy 工具和父进程崩溃恢复仍待实现；`Running` 仅指 SCM 状态，不代表 IPC worker 已认证就绪。帧序号和消息类型不是桌面操作授权；generation capability 和消息级允许操作表均未实现。真实 SCM worker、SYSTEM 桌面能力及 UAC/安全桌面仍待接线与验收。本轮未写入 SCM/ProgramData/防火墙，也未触发真实 UAC。详见[H01 设计与实现状态](h01-portable-elevated-helper-design-2026-11-25.md)。

2026-11-25 H01 独立服务入口切片：`chast-host.exe --service-name <实例名>` 现可直接进入 SCM dispatcher，并在 UI/网络初始化前分流。服务入口只接受完整、无额外项的实例与父进程身份参数；校验服务名、实例 ID、Session 0 LocalSystem 身份、本实例暂存映像受保护 ACL、父进程 PID/创建时间/session/image/user SID/logon SID，并在父进程退出或收到 SCM stop/shutdown 后结束。生命周期回归新增参数解析、损坏参数和跨实例拒绝覆盖。Release `chast-host`、生命周期测试和 `chast-host-parity-test` 构建通过，`portable_helper_lifecycle` CTest 1/1 PASS，`chast-host-parity-test --elevation` 通过；普通进程启动 dispatcher 探针按预期以 2 退出。服务入口没有初始化 IPC/桌面 worker，`SERVICE_RUNNING` 不等于 worker-ready；本轮没有创建 SCM 服务、写入 ProgramData、触发 UAC 或防火墙。受保护 staging/copy、恢复记录、真实 SCM 生命周期及 IPC capability/桌面桥接仍待。详见[H01 设计与实现状态](h01-portable-elevated-helper-design-2026-11-25.md)。

2026-11-25 H01 受保护暂存/copy 实现切片：新增 `stage_portable_helper_service_image` elevated-only API，内部生成随机实例 ID，源映像限定为当前进程 image path；共享机器状态目录只在首次创建时硬化，已存在目录必须通过受信 ACL 检查；随机实例目录拒绝碰撞，PE 复制拒绝 reparse、多硬链接、坏 header、超限映像及覆盖目标，复制后硬化并校验 ACL，失败只尝试清理本实例文件/目录。隔离临时目录回归通过当前测试映像字节一致复制、现存目标保留、非法 PE/硬链接源拒绝，生命周期另覆盖非零随机 ID。Release 三目标构建、生命周期 CTest 1/1 及 `chast-host-parity-test --elevation` 均通过。staging API 尚未接入 UAC/授权流程，本轮没有调用它或写 ProgramData；代码还未校验 Authenticode 发布者/签名，生产接线前须明确发布者固定策略。真实管理员 ACL、真实 SCM 生命周期、崩溃恢复、worker-ready/授权 IPC 与桌面桥接仍待。详见[H01 设计与实现状态](h01-portable-elevated-helper-design-2026-11-25.md)。

2026-11-25 H01 服务与映像清理生命周期：协调器清理已扩展为先停服务、删准确的实例 SCM 项，再确认 SCM 中该服务完全不可打开后，删除受保护的本实例映像并移除空目录。仍在运行/仍注册/marked-for-delete 的服务都会阻止映像删除；文件路径、ACL、reparse 和 hard-link 状态在删除前复核。服务删除失败或映像清理失败进入 `CleanupPending`，后续可重试；创建失败回滚同样保留名称冲突服务使用的映像。fake backend 覆盖服务先于映像的顺序、映像清理失败重试和名称冲突不误删映像，`portable_helper_lifecycle` CTest 1/1 PASS。此 Win32 SCM/ProgramData 清理代码尚未在真实管理员环境调用，H01 仍未端到端接线。

2026-11-25 H01 持久恢复记录切片：新增版本化 `recovery.dat`，记录绑定实例 ID、服务/映像派生路径及父进程创建时间和交互身份；独占临时写入、Flush、受保护 ACL 硬化后原子改名。SCM 服务启动和映像清理要求记录完整且与参数/规格相符；异常文件、reparse、硬链接、坏版本/长度或身份错配均失败关闭。定向 CTest 覆盖编码往返、错误 magic、截断/尾随数据和非规范规格，`portable_helper_lifecycle` 1/1 PASS；Release `chast-host`/生命周期/提权回归构建及 `chast-host-parity-test --elevation` 通过。**该切片完成时**尚无开机/启动扫描器自动发现并恢复孤儿实例，也没有真实 ProgramData/SCM 管理员环境证据；记录不是 UAC 授权凭证，staging 仍未接入本地授权流程。

2026-11-25 H01 启动恢复扫描接线：正式 `chast-service` 在报告运行后以独立后台线程调用受保护 ProgramData 恢复扫描；单实例恢复失败只记录日志，不阻断正式服务。服务停止/关机后，线程在当前 SCM 操作结束后停止继续处理记录，正式服务等待扫描线程退出并持续报告 STOP_PENDING。恢复 API 增加可注入取消检查，fake backend 验证取消后不触碰后续实例。Release `chast-service` 与生命周期测试构建通过，`portable_helper_lifecycle` CTest 1/1 PASS，`git diff --check` 通过；`--run` 在 SCM 外按预期拒绝。尚未在真实已安装服务下观察日志，也未创建/清理 ProgramData 或 SCM 实例，因此仍需隔离管理员实机验收。UAC/本地授权接线、真实 SCM lifecycle、IPC worker-ready/capability、桌面桥接、升级回滚和安全桌面验证仍待。详见[H01 设计与实现状态](h01-portable-elevated-helper-design-2026-11-25.md)。

2026-11-25 S05/S06/v33 代码检查点：当前分支已接入 schema v29–v33 的安全告警、邮箱验证、邀请和登录设备白名单。S05 有独立事件、去重/留存、分页筛选/CSV、权限范围管理台查询及带数据库租约和退避重试的邮件队列；S06 有邮箱验证和一次性邀请流程；v33 的用户签名设备证明已接入密码、LDAP、OIDC、WebAuthn，Windows 安装密钥由 DPAPI 保护。管理台账号列表补上管理员设备恢复 UI，支持查看、登记公钥、启停白名单和撤销设备；读取要求全局 `accounts.read`，更改要求 `accounts.write`，启停和撤销会话影响有明确确认，无受信设备时不能启用。

2026-11-25 融合后门禁复跑：`go test ./... -count=1 -timeout 10m`、`go vet ./...`、Flutter `flutter test --no-pub` 881/881、`flutter analyze --no-pub`、管理台 Node 53/53、`session_tab_model` CTest 1/1 及 `cmake --build build/root --config Release --parallel 4` 均通过；Windows Flutter C03 Release loopback 因第二轮重连后的 tooltip 空文本失败（见上方记录），因此 C03 端到端门禁未通过。安全密钥回归测试使用无设备证明测试 provider 隔离既有 WebAuthn 流程，设备证明接口有独立覆盖。`CHAST_TEST_POSTGRES_DSN` 未设置；PostgreSQL 实库、真实 SMTP、部署浏览器和双机设备恢复仍待验收。S05 的告警目录、风暴/重复投递语义和生产留存值待运维确定。详情见[账号安全与企业治理计划](account-security-priority-and-next-plan-2026-11-24.md)和[S05 告警状态](s05-alarm-status-2026-11-24.md)。

2026-11-23 状态归档：本次快照涵盖 C03 多连接标签/媒体隔离与关闭失败回滚、C04 剪贴板/触控板/快捷键、H03 查看端队列恢复及主机入站录制归档整链、O01 活动连接快照 CAS，以及相关测试和设计记录。现有记录中的 Flutter 870/870、音视频/窗口 loopback、H03 Release CTest 4/4 与 Go 定向测试均按各自日志和范围成立；归档操作不代表重新运行全量门禁。C03 历史查看端退出根因仍未定位，Win32 关闭消息失败注入待做。C04 真实应用剪贴板、键盘/IME、触控板和音视频/物理显示场景待实机验收。H03 正式服务/生产部署、双机及源 MP4 清理策略待定。W1 正式 SCM/安全桌面/防火墙回滚与双机/生产数据库验收仍未关闭；W3 A01–A03、S04–S06，W4 H01–H02/O01–O04 及 W5 发布候选门禁仍按路线图待实施或验收。测试夹具和构建产物不属于源码快照。

2026-11-23 A01 安全设计复核：现有密码同步设计已补充密钥目录认证/主动服务端边界、密文上下文与回滚检测、恢复包 KDF、元数据泄露和撤权限制。尚未决定端到端不可恢复或服务端托管可恢复；协议、数据库和 UI 均未实现，等待产品取舍及安全评审。

2026-11-23 O01 核对：活动连接快照已加数据库条件写入（CAS），拒绝跨 Store 的迟到写入；SQLite 双 Store 的快照及录制配额回归均通过。新增 PostgreSQL 快照回归因未配置 `CHAST_TEST_POSTGRES_DSN` 而跳过；PostgreSQL 跨 Store 录制配额并发尚缺实库回归，不能用 SQLite 结果替代。录制分片 offset、配额预留和文件提交仍由各实例的文件目录和进程内锁协调；共享数据库不能替代共享存储及跨实例写入协调，当前不应让多个服务进程共同写入同一个录制目录。

2026-11-23 H03 主机入站归档本机链路完成：服务端签名分片 API、Go worker、C++ 封口回调/原子任务 producer、默认关闭的 opt-in/队列接线，以及真实 `chast-host`→C++ `TunnelProcess`→`chast-tunnel`→服务端整链均已实现并通过隔离 loopback。整链用合成画面和真实软件 VP9，确认探针收帧、会话结束时 MP4 封口、归档 owner/大小/SHA-256 正确且源文件保留；测试不会捕获真实桌面，并只读取已有用户证书。较早 E2E 另连续两轮验证编译态 tunnel、私有 TLS CA、CONNECT 代理、父进程强杀后的子进程回收和 4 MiB offset 恢复；Release CTest 4/4、Go agent/tunnel 测试和 vet 通过。正式服务/ProgramData/防火墙、生产部署、双机验收及源 MP4 清理策略仍待。详见 [H03 主机归档](host-recording-archive-design-2026-11-23.md)。

2026-11-22 20:30 C04 SVG 剪贴板：CCLB v5 / 协议 v78 和 Windows `image/svg+xml` 已实现，静态 SVG 通过 XmlLite 检查，活动内容、外部引用和超限数据拒绝；双向 ViewerEngine/HostServer/QUIC 回环、CCLB 恶意输入拒绝及私有原生剪贴板探针通过。探针还发现活动 SVG 与有效文本/PNG 共存时错误地使整个剪贴板读取失败；现改为只略过无效 SVG、保留有效文本/PNG，并在私有窗口站真实回归通过。协议与剪贴板定向 CTest 2/2、Flutter 全量 861/861、`flutter analyze --no-pub` 与 Windows Release 产品构建均通过；Release 用同一 CMake clipboard target 成功链接 XmlLite（构建日志 `c04-svg-flutter-release.log`，237.6 秒）。真实应用间 SVG 粘贴仍待本轮外部/交互验收；不会把 native 探针等同于应用兼容性验收。剪贴板格式与预算历史页见[PNG 专题](clipboard-images-2026-11-13.md)。

2026-11-22 22:43 C04 快捷键 Release loopback 复验通过，覆盖 Ctrl+F7 开关远控、原生捕获开启时 Ctrl+F8 切换统计、关闭远控后普通 A 不发往主机，以及关闭原生捕获后 Ctrl+F8 经 Flutter 回退切换统计。最终结果字段均为 true，证据 `build/native-runtime-20261122-224253-d0c0572b/acceptance.log` 与 `run-1962698155/flutter-input-results.json`；快捷键相关 Flutter 定向测试 12/12、`flutter analyze --no-pub`、键盘 CTest 1/1 通过，Windows Release loopback 完成。随后在最终工作区重跑 `D:\flutter\bin\flutter.bat test --no-pub`，**870/870 PASS**。22:04 的失败已由本次复验覆盖，但仍保留为历史诊断。物理键盘/IME、真实应用间剪贴板及触控板验收仍待。

2026-11-22 23:48 C03 标签关闭三批 trace 压测：在 23:41 trace-disabled 三轮通过后，启用详细生命周期 trace 连续三批 Release loopback；每批三轮关闭/重连、兄弟连接保持及末尾管理台精确断开全部通过。三批验收各 51–58 秒 PASS，每份轨迹均记录 3 个 `close-tab-request`、3 个 `prune-child` 和 6 个 `viewer-bridge-close`；累计 27 个后续实际关闭周期通过。最新证据目录分别为 `build/native-runtime-20261122-234512-64332027/run-4193874309/`、`build/native-runtime-20261122-234624-b6c7a031/run-406761616/`、`build/native-runtime-20261122-234730-8cc8cf40/run-4281782606/`。12:05/12:08 的历史查看端进程退出仍未解释；未复现不能视作修复，关闭行为未改。失败时会采集查看端退出码，`GetMessage` 错误以失败码退出。另一次前置 access token 认证失败未进入关闭步骤，不计入。完整边界见[标签页拆分记录](multi-session-tabs-design-2026-11-21.md)。

2026-11-23 C03 关闭消息投递失败回滚：`CloseTab` 现在在 `PostMessage(WM_CLOSE)` 失败且子 HWND 仍有效时恢复关闭前的模型状态、选中项和可见性；失效 HWND 继续交由 prune。Release 模型 CTest 1/1 及 Flutter Windows runner Release 构建通过。追加正常关闭回归 3 轮通过，累计 49 个关闭/重连周期；真实 Win32 失败注入未做，历史 12:05/12:08 查看端进程退出原因仍未定位，该窄修复不代表关闭冲突整体解决。

2026-11-22 22:04 C04 快捷键 Release loopback 首轮失败：Ctrl+F7 开启/关闭远控、原生捕获期间 Ctrl+F8 切换统计、远控关闭后阻止普通 A 输入均通过；关闭原生捕获后 Ctrl+F8 未经 Flutter 回退切换统计。临时诊断观察到会话 Flutter 焦点为主焦点，但该次 F8 没有进入 Flutter `KeyEvent`；焦点恢复试改未解决且已撤回。该失败已由 22:43 复验覆盖，证据仍保留在 `build/native-runtime-20261122-220340-4c69e343/`，具体见[审核修复记录](review-fixes-2026-11-21.md)。

2026-11-22 20:00 C04 边缘滚动/样式：视图设置提供滚动条、自动滚动、边缘滚动三种模式；边缘感应范围 20–150 px、默认 100 px。自动/边缘平移后，鼠标命中位置重新按当前画布位置映射，正常滚轮仍只发往远端，不会同时滚动画布；设置按连接目标保存，主窗口和显示子窗口共用。新增滚轮路由、自动定位、静止指针连续边缘平移、偏好回退/保存及显示子窗口设置回归。原全量 Flutter **861/861**、`flutter analyze --no-pub`、Windows Release 构建通过。此后 C04 SVG 与可配置快捷键也已实现，最终总门禁/构建结果见快捷键本轮记录；真实触控板/触摸设备、键盘及应用剪贴板输入仍待物理验收。

2026-11-22 C04：主会话“发送文本”支持显式读取本地纯文本剪贴板，预览/编辑后确认发送；触控板速度、边缘/自动滚动、滚动条样式、SVG 剪贴板及五项可配置会话快捷键均已实现。快捷键可录制/保存/清除，原生 hook 和 Flutter 焦点回退闭环接通；Windows Release 编译、键盘 CTest 和快捷键 UI/桥接测试通过。物理键盘/IME、真实应用剪贴板互通和触控板验收仍待，具体切片结果见[审核修复记录](review-fixes-2026-11-21.md)。

2026-11-22 11:53 C03 显示子窗口生命周期：Flutter Windows Release 单机 loopback PASS。两个标签各打开一个显示子窗口，窗口随父标签切换显隐；断开 A 后 A 子窗口关闭且 A 状态为 ended，B 子窗口及兄弟标签保留，B 仍 streaming 且解码计数 1→115。fixture 使用合成双逻辑显示器并将两者映射到同一只读本机桌面捕获，不改变系统显示拓扑。首轮发现测试 TSV 未写终态快照，已在测试门控下补写并复验通过。证据 `build/native-runtime-20261122-115352-3e16ff0f/flutter-display-window-163144787/flutter-display-window-lifecycle.json`。这关闭合成拓扑下的父子窗口生命周期 loopback 项，不代表物理多屏/DPI/热插拔或双机；标签关闭逻辑未改。

2026-11-22 12:08 C03 标签关闭诊断：`-Flutter -MultiWindow` 当前构建再次失败；原生轨迹确认关闭请求已投递，随后 Flutter 查看端进程退出，15 秒后的进程快照无顶层窗口。两次新运行均未记录 child prune/host-close。失败产物 `build/native-runtime-20261122-120816-e7534627/run-32384536/` 与 `build/native-runtime-20261122-120536-a19d6fdb/run-2777428185/`。退出调用来源仍未定位；没有修改标签关闭路径，问题继续作为 C03 阻断项。

2026-11-22 H03 查看端恢复验证：主会话与显示子窗口在 native 开始录制前持久化 intent；显示子窗口接收非令牌上传归属，父会话按录制路径认领并沿用任务 ID。首页无会话冷启动会 drain，按当前账号/Viewer Strategy 授权，worker lease 防止多进程并行上传。真实 Flutter Windows 进程在 PATCH 响应挂起时被强杀，重启后实际 HTTP GET 恢复 offset、完成归档并清空本地队列；服务端校验 44 字节及 SHA-256 `c88cc81a20a6afbbe8905c05522cb1e6545bcf573bdde9273c149ef02d895546`。未封口 MP4 现显示首页提示；intent 和媒体文件均无限期保留，不自动上传或删除，封口后自动恢复；扫描使用非阻塞锁且不延迟 drain。Flutter 全量 **848/848**、analyze 及当前产品 Windows Release 构建通过。证据：`build/test-temp/chast-upload-app-e3ff6e4998fc45279fcd19ea5aee56a1`。这关闭的是 H03 查看端恢复延伸；原计划中的主机入站集中归档、服务端部署及双机验收仍未完成/后置。详见[审核修复记录](review-fixes-2026-11-21.md)。

2026-11-22 C03 显示子窗口归属回归：将标签会话的子窗口可见性/归属注册器独立为 Win32 组件，修正同一 HWND 重新注册到另一会话时旧归属不更新的问题。新增 `session_tab_visibility` 测试覆盖 A/B 切换、唯一活动归属、窗口重新分配、旧 owner 删除不影响新 owner、失效 HWND 清理及清除活动会话后隐藏全部窗口。显示归属与标签模型、显示准入、焦点及防休眠租约定向 CTest 6/6 PASS；Flutter Windows Release 构建通过（18.3 秒）。这仍不是实际多显示器窗口随父标签关闭的 Release 验收。防休眠引用计数和 Windows PowerRequest 测试本轮各 1/1 通过，但仍缺真实双标签连接/断开生命周期联测。标签关闭流程未改。

2026-11-22 C01 保活租约：原生桥现仅允许 connecting/handshaking/streaming 会话获取；已持有租约不重复计数，ended/failed/idle 拒绝迟到获取，释放不受会话状态限制。另修复 Flutter 页面首次观察会话终态后未及时释放租约的问题，结束卡片仍可保留用于重连。`viewer_awake_request` 策略测试及窗口/租约 CTest 6/6 PASS；双 Flutter Release loopback 证实初始 2 个 lease，首会话 ended 后为 1 个、兄弟仍 streaming，末会话 ended 后为 0 个。Flutter 全量测试 829/829、`flutter analyze --no-pub` 和双标签原生进程验收均通过。物理 OS 电源行为、长时实机及双机仍待验收；证据见审核修复记录。

2026-11-22 10:20 C03 真实 Flutter 双标签音频 UI 路由：Release loopback 专用 fixture 对两会话发送合成静音 Opus；接收端启用测试专用丢弃型播放器，不打开物理音频输出。实际操作工具栏音量按钮并逐会话检查原生快照：静音 A 不影响 B，静音 B 不影响 A，取消 A 静音后 B 仍静音；两路在切换过程中持续解码。连续 3/3 PASS（10.02、9.97、10.04 秒）；首次记录解码 A/B=36→669、46→468，最终状态为未静音/静音。最近证据 `build/native-runtime-20261122-102955-c7e0c926/flutter-tab-audio-3261278057/flutter-tab-audio-isolation.json`，另两轮证据在 `build/native-runtime-20261122-102052-69d33a82/`、`build/native-runtime-20261122-102701-8db1c64a/`。这证明 Flutter 按钮到选中会话的静音状态路由，不证明真实声音、物理设备切换或音质/AEC；标签关闭冲突仍单独保留，未改关闭逻辑。

2026-11-22 C03 标签页基础验收：同一 Release Flutter 进程内建立两条 loopback 会话，真实切换原生标签 `1 -> 0 -> 1` 并逐次确认只有一个会话子窗口可见；按 connection ID 精确断开 A 后 B 仍 active。验收 PASS，证据 `build/native-runtime-20261122-062558-0470e5b3/run-3045181632/`。不覆盖按键释放/后台输入拒绝、切换期间持续收流、标签级音频与录制 UI、标签关闭/重连或多显示器子窗口生命周期。

2026-11-22 08:31 C03 双标签输入/媒体连续性补验：Release loopback 证明切出 A 时远端按键释放先于物理清理、隐藏标签鼠标输入不抵达主机；返回 A 后 B 隐藏运行 1,867 ms，收包/解码/发布各 253→309、录制样本 220→276，状态仍为 streaming/recording。证据 `build/native-runtime-20261122-083058-21a139b0/run-1332851354/`。选中标签关闭 07:07 曾通过，但 07:59、08:04 两次后续验收出现标签/窗口计数归零；关闭结果冲突，当前不判通过，也未改关闭逻辑。逐标签媒体 UI/音频隔离、关闭失败回滚及其它 C03 实机/显示场景继续待验。

2026-11-22 08:59 C03 逐标签录制状态隔离补验：Release loopback 新增 A/B 独立快照断言，A streaming 且 `recording=false`/样本 0，B streaming 且 `recording=true`；B 隐藏 1,863 ms 时收包/解码/发布各 248→304、录制样本 215→271，B 结束快照样本 279。输入释放和隐藏输入拒绝同轮通过。证据 `build/native-runtime-20261122-085831-bf256fe4/run-381049699/flutter-tab-recording-isolation.json`。这证明原生会话录制状态隔离，不证明录制按钮 UI 或音频播放/静音隔离。关闭问题仍单独保留、不改关闭逻辑。

2026-11-22 09:29 C03 逐标签录制 UI 与媒体连续性补验：Release loopback 通过，A 保持 streaming 且不录制，B 保持 streaming/录制；B 隐藏 1,913 ms 时收包、解码、发布各 247→312，录制样本 209→273。A/B 工具栏截图的录制按钮高亮像素分别为 0/126，Go 验收强制断言 A≤4、B≥20，并验证截图文件存在。切换按键释放、隐藏输入拒绝同轮通过；Flutter `display_menu_test.dart` 42/42 通过。证据 `build/native-runtime-20261122-092823-23ee1ae7/run-166595522/flutter-tab-recording-isolation.json`。UI Automation 仍无法枚举 Flutter 控件；按钮状态由截图像素断言验证。此轮不证明音频隔离：本机 loopback fixture 的主机配置为 `audio=false`。

2026-11-22 09:51 C03 原生双会话音频隔离：扩展 `viewer_audio_video_isolation`，两条 ViewerEngine 连接同一 loopback host 的合成静音 Opus 源，各使用独立内存播放器。A 静音/B 播放、反向切换后各自的播放器状态与输出块计数均独立；静音不停止解码，慢视频 5 FPS 下两路音频持续，断开 A 后 B 继续，重连正常。CTest 1/1 PASS（7.94 秒），日志显示解码 A/B=220/224 包、重连 117/117 包、视频帧 A/B=12/12、错误 0；测试未打开物理音频输出。Flutter Windows Release 构建 PASS（58.1 秒）。该轮只证明 ViewerEngine 级隔离；真实 Flutter 标签音量按钮路由由 10:20 专项验收补齐。

2026-11-22 录制撤权提示：主会话和显示子窗口现将旧 native 错误文本 `the host revoked recording` 归类为权限撤销，而非录制失败；其余录制错误仍保留原提示。两个页面的 widget 回归、错误分类测试、英语翻译回归通过；Flutter 定向 10/10、全量 828/828、analyze 无问题、`l10n_test.dart` 7/7。当前没有新增 Release 实际弹窗采证，详见[审核修复记录](review-fixes-2026-11-21.md)。

2026-11-21 21:30 补充（当时状态）：已修复录制撤权后仍继续写入，以及 Flutter 显示子窗口初始化期间无障碍查询导致进程崩溃。新录制回归保留修复前失败、修复后 3/3 通过；Flutter 定向 10/10、analyze、最终 Release 构建及双 engine 初始化回归通过。真实隔离窗口已验证子窗口创建、开始录制、撤权停止、恢复权限不自动录制及子窗口关闭后主窗口存活。撤权提示当时仍显示通用“录制失败”及英文原因；2026-11-22 的分类/本地化修复和 widget 证据见上段。完整设备/重连/多会话释放及物理显示验收仍缺；多连接标签页现有基础实现和切换证据，但完整输入/媒体/关闭重连隔离仍待验收；H03 队列持久化和 C04 尚未实现。证据及边界见审核修复记录；代码仍未提交、推送。

2026-11-21 20:50 最新结果：持续迟到播放重锚、拥塞降码率反升及 AVC 隐含最低码率已修复。候选 C 固定构建的干净/弱网各五轮组合门禁 10/10 PASS；干净 skew +7.5～+33.6 ms，弱网最大视频帧龄 962～1801 ms。原生单测 9/9、集成 4/4、AVC 重配置/解码三场景、Flutter 定向 6/6 和 Release 构建均通过。弱网仍有 140～305 ms 最大设备断档，实验通过不代表真实音质达标。C03 的实际窗口/设备/显示验收及多连接标签页尚未完成；下文候选 A/B 与后续失败均保留为历史证据。逐轮指标、哈希和日志见审核修复记录。

2026-11-20 分支核对：通过指定 SOCKS5 代理同步后，确认当前分支完整包含 `main` 与 `codex/s03-role-console-2026-11-17`，本地/远端 HEAD 均为 `172516e`；现有未提交改动保留。后续使用此分支，具体拆单与退出条件见 [分支基线与下一步执行计划](branch-baseline-and-next-actions-2026-11-20.md)。

## 1. 当前结论

项目尚未达到“功能与性能全面对齐 RustDesk”的完成条件。当前为 **W1 外部验收待收口，W2 本机功能已大体实现、场景验收继续推进**：审核发现的明确缺陷已修复，S01、S02、S03 的必要代码链路已接通，并取得本地 SQLite、PostgreSQL、浏览器和 Windows 原生进程证据；安装/卸载事务与防火墙 COM 后端已继续加固并通过 fake/隔离回归，但 W1 的双机、正式 SCM 服务、真实 COM 写入回滚和生产环境验收尚未完成。C01–C04 已有主要实现和本机自动化/loopback 证据；C03 标签关闭的历史进程退出原因仍未定位，物理显示、音频、输入设备等场景验收继续待做。W3 企业治理与 W4 生产能力仍有明确实现缺口。

截至 `34e5a9b`，当前分支已包含 schema v29–v33 的 S05 告警、S06 邮箱验证/邀请和登录设备白名单实现。PostgreSQL 实库、真实 SMTP、部署浏览器及双机设备恢复仍待验收；告警运营语义亦待确认。

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
| S05 安全告警 | 代码与自动化门禁已通过；实环境验收待完成 | schema v29 告警事件、v31 持久邮件任务及偏好已接通；PostgreSQL 实库、真实 SMTP/部署浏览器、事件目录、风暴/重复投递语义及生产留存值待确认/验收 |
| S06 本地邮箱验证与用户邀请 / v33 登录设备白名单 | 代码与本地门禁已通过；外部验收待完成 | 邮箱验证、一次性邀请和管理员设备登记/恢复/启停/撤销已提交；PostgreSQL、真实 SMTP、部署浏览器及双机设备恢复仍待验收 |

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
| C01 | 部分完成 | 连接前默认仅查看偏好、主机 LAN 发现设置及查看端会话期间的 Windows 防休眠请求均已实现；请求由进程级引用计数共享。双 Flutter Release loopback 已验证首会话结束释放单个租约、兄弟租约保留，以及末会话结束后租约归零；迟到获取竞态和 Flutter 终态释放也已修复。物理电源行为、长时实机及双机验收仍待完成 |
| C02 | 已实现，单机自动化通过；场景验收待完成 | 按设备保存 5–1000% 视图倍率和 25–400% 光标倍率，支持自适应/原始大小基准；主会话、桌面多显示器布局及独立显示器窗口已接入。自动化覆盖源画面输入映射、光标热点/独立倍率、普通滚轮远端转发与 Shift+滚轮本地平移，并覆盖逻辑窗口尺寸固定时 DPR 从 1 到 2 的相对输入倍率更新。真实混合 DPI、旋转、全屏及多屏人工验收仍待完成 |
| C03 | 部分实现；关闭生命周期仍为未关闭风险 | 已实现窗口布局恢复、独立连接/录制隔离、子窗口媒体菜单和多连接标签基础容器；音视频组合门禁 10/10 PASS。两标签 Release loopback 已验证切换/可见性、输入释放/后台拒绝、兄弟隐藏时持续收帧/解码/发布/录制、逐标签录制/静音 UI、双标签 lease 首断/末断释放，以及合成双逻辑显示器下子窗口随父标签切换和断开 A 后 B 持续解码。12:05/12:08 曾复现关闭后查看端进程退出；之后累计 49 个实际关闭周期通过，但旧失败根因仍未知，不能据未复现宣告修复。Win32 `PostMessage(WM_CLOSE)` 明确失败时的模型回滚已实现并通过单测，正常关闭/重连 Release loopback 追加 3 轮通过，真实失败注入验收待完成。另修复录制撤权及窗口初始化崩溃，真实子窗口创建/录制撤权/关闭已有实测，撤权提示分类/本地化有 widget 回归。真实语音/AEC/降噪、物理播放设备/电源、物理多屏/热插拔、混合 DPI/全屏仍未完成；Release 撤权弹窗尚未专项采证。完整证据见 [审核修复记录](review-fixes-2026-11-21.md) |
| C04 | 功能已实现；本机 Release loopback 通过，真实设备验收待完成 | 主会话支持读取本地纯文本、预览/编辑后作为 Unicode 键入；双指平移速度、边缘滚动/样式按设备目标保存；CCLB v5 / 协议 v78 静态 SVG 有安全校验、原生读写和双向 QUIC 回环。五项会话快捷键已接入偏好、录制 UI、Windows hook、Flutter 回退与动作权限校验。2026-11-22 22:43 快捷键 Release loopback 通过，覆盖原生捕获开启/关闭及 Flutter 回退路径；快捷键定向 Flutter 12/12、最终全量 Flutter 870/870、analyze、键盘 CTest 1/1 通过。真实应用间 SVG 粘贴、键盘/输入法、剪贴板按键发送及物理触控板验收仍待 |
| H03 查看端与主机归档 | **本机实现与整链 loopback 验收完成** | 查看端真实 Flutter Windows 进程强杀后可恢复服务端 offset、哈希归档并清空队列；主/显示子窗口 intent、账号/服务器隔离、编号分段幂等恢复、无会话冷启动 drain 和未封口提示均有覆盖。主机入站归档已由真实便携 `chast-host` 加载 opt-in 设置、合成画面 VP9、封口 MP4，并经其 `TunnelProcess` 启动的真实 tunnel 完成 owner 绑定归档和哈希核对；源文件保留。正式服务/ProgramData/防火墙、生产部署、双机及源文件自动清理策略仍待 |

### 3.3 W3 企业协作与治理

| 任务 | 待实现范围 |
| --- | --- |
| A01–A03 | 地址簿密码端到端加密设计，个人与共享簿密钥封装、恢复、轮换、撤权；独立命名共享地址簿；跨设备密文同步，服务端不得保存地址簿明文密码 |
| S04 | Control Role 命名角色及继承/启用/禁用三态覆盖，明确与 Strategy、访问票据和权限上限的求值顺序 |
| S05 | 独立告警、管理台查询/筛选、schema v31 持久邮件任务与后台 worker 已实现；待确定事件目录/风暴策略、重复投递语义和生产留存值，并完成 PostgreSQL、真实 SMTP 与浏览器验收 |
| S06 / v33 | 邮箱验证、一次性邀请、用户设备白名单及管理员登记/启停/撤销界面已实现；Go 全量、vet、Flutter 880/880 与 analyze、管理台 Node 53/53 通过；PostgreSQL、SMTP、部署浏览器和双机设备恢复仍待验收 |

### 3.4 W4 主机与生产部署

| 任务 | 待实现范围 |
| --- | --- |
| H01 | IPC framing/Named Pipe、注入式生命周期协调器、Win32 SCM adapter、独立 SCM service entry、elevated-only 暂存/copy、持久恢复记录、严格的本实例服务/映像清理代码及正式 `chast-service` 启动后的后台恢复扫描接线已完成；尚需 UAC 授权流程、隔离管理员 SCM/ProgramData 实机验收、IPC worker-ready/capability/撤权、桌面桥接、升级回滚和真实 UAC/安全桌面验收 |
| H02 | 扩展虚拟显示后端、自定义分辨率、驱动签名/分发条件及多 GPU、HDR、热插拔回退 |
| H03 | 本机集中上传、断点续传和幂等归档已实现并通过真实便携主机 loopback 整链；生产服务/部署与双机验收后置，源 MP4 自动清理/保留策略仍待定义 |
| O01 | 活动连接与指令已落库，快照 CAS 已修复且有 SQLite 双 Store 回归；仍需 PostgreSQL 双 Store 验证、共享限流、录制存储和跨实例写入协调，以及实际两实例指令投递与故障接管验收 |
| O02 | 已有 PostgreSQL 后端和本地集成证据；生产 TLS、最小权限、数据库与录制文件一致恢复、故障切换、滚动升级和监控仍待。先确定 RPO/RTO 和既有会话连续性目标，再做生产拓扑演练 |
| O03 | 面向受限网络的自有协议 API 回退通道；需求若是 RustDesk 官方代理协议，则另立条件项目 |
| O04 | 定制客户端配置/品牌管理、构建产物关联、签名、下载、升级与回滚 |

### 3.5 W5 发布候选验收

完成真实 Windows 功能矩阵、双机及复杂网络、RustDesk 同条件 A/B、PostgreSQL 多实例容量和故障接管，以及完整 Flutter、Go、Windows Release、原生 CTest 和浏览器门禁。缺少的硬件、网络或生产环境继续明确标为待验收，不以本地回环或模拟回执替代。

## 4. 执行顺序

执行拆单沿用 [分支基线与下一步计划](branch-baseline-and-next-actions-2026-11-20.md)，账号安全功能的产品优先级和验收计划见[账号安全与企业治理计划](account-security-priority-and-next-plan-2026-11-24.md)，最新退出条件以 [2026-11-21/22 审核修复记录](review-fixes-2026-11-21.md) 为准。标签关闭冲突已复现为关闭子标签后查看端进程退出，但退出来源未定位；按既定约束不改关闭路径，保留为未解决项。H03 查看端 HTTP 强杀重启及主机入站录制真实 `chast-host`/tunnel 整链均已有单机 loopback 证据；正式服务、生产部署及双机后置，源 MP4 自动清理策略尚未定义。C04 已实现剪贴板按键发送、触控板、SVG 和可配置快捷键；按设备条件继续补物理键盘/IME、真实应用剪贴板、触控板、显示、音频和电源验收。W1 环境验收继续保留，双机按既有安排后置。

1. 完成 W1 外部验收并汇总剩余缺口；环境未就绪时继续补齐可验证的测试工具、失败路径和文档，不虚报发布门槛通过。
2. W1 外部验收仍未收口；按 2026-11-20 用户决定，双机验证后置。C01 默认仅查看偏好、查看端会话保活租约、LAN 发现与 host/viewer Strategy 配置优先级已完成本机定向测试及 UDP 回环复核（本节 2026-11-25 记录）。仍保留真实 OS 电源行为、长时实机及 W1 外部验收待办；不把单测/回环当作物理设备或双机证据。
3. C03 最新固定构建已通过干净/弱网各五轮组合门禁，保持原有 ±40 ms 稳定链路 skew、3000 ms 弱网帧龄及默认码率。标签切换输入安全、兄弟持续收帧/录制、A/B 原生录制状态隔离、合成静音源下 Flutter 音量按钮路由、双标签首断/末断 lease 释放及合成双显示器拓扑下父子窗口关闭/兄弟存活均已有 loopback 证据。标签关闭仍作为独立问题不改关闭逻辑；真实语音/AEC/降噪、物理音频设备与电源行为、物理混合 DPI/旋转/热插拔依环境单列。
4. 并行开展 A01 密码同步安全设计和 O01/O02 生产部署设计；设计通过评审后再实现共享地址簿、多实例协调和生产数据库能力。
5. 随后按[账号安全与企业治理计划](account-security-priority-and-next-plan-2026-11-24.md)完成 S05/S06/v33 的 PostgreSQL、SMTP、部署浏览器和双机设备恢复验收；再实施 A02/A03、S04、H01/H02、O03/O04 和 H03 源文件保留策略，最后执行 W5 发布候选门禁。

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
