# C03 多连接标签页实施拆分

2026-11-25 20:55 融合后 C03 Release loopback：完整运行未通过。初始两标签及首次关闭/重连后的长标题 tooltip 文本/隐藏断言通过；第二轮重连后，标签 `#4` 的 tooltip 断言得到空文本（鼠标仍在 tab-control 上），用例退出 1。日志/产物：`build/native-runtime-20261125-205510-fa1339bf/run-2331120209/`。本轮包含此前新增的同步重绘代码，说明 tooltip 在重连路径仍有未解决问题；需定位 popup 生命周期/显示时序，不能放宽断言。历史标签关闭进程退出根因仍独立未定位。

2026-11-25 Release 20 轮复验：当前源码 Flutter Release loopback 完成 20/20 次“关闭选中标签→兄弟连接保持 active→重连关闭标签”，最后管理台精确断开通过。生命周期 trace 记录 20 组 `close-tab-request`、`close-tab-post`、`prune-child`，runner 共 142 条事件，未见窗口集归零、宿主销毁或消息泵退出/错误。证据 `build/native-runtime-20261125-094542-a80c0caa/run-1612339926/`。未复现 2026-11-22 进程退出故障，也未命中 Win32 投递失败分支；根因继续未定位，不据本轮压力通过关闭缺陷。

2026-11-24 23:28 长标签悬停提示自动化文本/显隐断言通过，并替换原生 tab-control tooltip 方案：owner-drawn 标签在裁切时显示深色、自动换行的完整标题与消歧尾缀；鼠标移出或宿主失焦时隐藏。loopback fixture 注入长设备名并检查提示窗口文本与 caption 相等。Windows Flutter Release 双连接验收 50.85 秒 PASS，含创建、唯一标签名、长提示、切换、三轮关闭/重连、兄弟连接保持及精确断开。**专项截图捕获不可靠：截图呈现底部工具栏而非提示文本；之后增加的 `RedrawWindow` 同步重绘改动尚未回归，故 tooltip 视觉证据仍待补。**运行记录 `build/native-runtime-20261124-232659-3bd5b908/run-2698131831/`。单机 loopback 不代表双机、屏幕阅读器或物理显示验收。

2026-11-24 21:14–21:18 压力复验：Windows Release Flutter loopback 连续 20 轮关闭选中标签/重连，兄弟连接每轮保持 active，末尾精确断开通过，耗时 221.01 秒。trace 记录 20 组关闭、子窗口销毁和 prune 事件，未见消息循环退出或窗口集归零。证据 `build/native-runtime-20261124-211412-d46dbbfa/run-983904780/`。未复现 11-22 历史进程退出，不能据此认定根因已解决。

2026-11-24 21:38 标签可辨识性改进：同名标签优先显示经校验的服务器 Host ID 末 6 位；直连、缺失 ID 或短尾冲突时退回进程内稳定标签序号。只使用公开设备标识，不展示凭据；关闭同名标签后重新计算消歧。标签名回归、Flutter 参数回归、Windows Release 构建及真实双连接/三轮关闭重连 loopback 通过（51.19 秒）。

2026-11-24 21:45 标签名端到端断言：原生 loopback helper 通过跨进程 `TCM_GETITEMW` 读取实际 tab captions，并断言创建及关闭/重连后标签数匹配且 caption 唯一。Release loopback 30.82 秒 PASS，兄弟连接持续 active，末尾精确断开通过；证据 `build/native-runtime-20261124-214529-ba6f4967/`。

2026-11-24 22:00–23:28 长标签提示迭代：早期版本保留净化后的完整标题并按宽度省略，但原生 tab-control tooltip 实际返回空文本；最终改为自绘 popup，并由真实 hover 文本、移出隐藏和截图验收替代。`TCM_GETTOOLTIPS` 仅确认控件存在，不作为提示功能验收证据。

2026-11-24 21:49 标签改动后 Flutter 全量回归：`flutter test --no-pub` 871/871 PASS，`flutter analyze --no-pub` 无问题；不替代全量 C++、硬件或双机门禁。

2026-11-24 真实 Win32 失败注入：首轮误用 `HWND(-1)`，观察到宿主 `WM_CLOSE`，未通过；改用 `0xDEADBEEF` 后，真实 `PostMessageW` 对无效 HWND 返回失败，host 回滚通过。前后标签数、窗口数、选中项不变且查看端进程存活，随后同进程 3 轮正常关闭/重连及精确断开通过。注入只覆盖无效 HWND，不模拟消息队列满载，也不解释 11-22 历史进程退出。证据 `build/native-runtime-20261124-193406-b320b956/run-243170596/`。

2026-11-23 05:42 正常关闭路径回归：失败回滚改动后再次运行 Flutter Windows Release `-MultiWindow` loopback，50.95 秒 PASS；同一进程完成 3 次关闭/重连，兄弟连接每轮保持 active，精确断开通过。trace `build/native-runtime-20261123-054108-fd4b2f28/run-1570086336/session-tabs.tsv` 记录 3 组 `close-tab-request`/`close-tab-post`/`prune-child`。累计 49 个历史问题后的关闭周期通过；未注入失败分支，旧故障根因仍未知。

2026-11-23 关闭失败回滚补充：若子窗口仍有效但 `PostMessage(WM_CLOSE)` 失败，host 现在恢复 closing 标记前的选中标签及可见状态；失效 HWND 保留给 prune 清理。模型 Release CTest 1/1、Flutter Windows runner Release 构建通过。2026-11-24 已补无效 HWND 的真实 Win32 失败注入并验证 host 集成回滚，但 12:05/12:08 历史进程退出原因仍未定位，不能据此判定 C03 整体关闭冲突解决。详情见[审核修复记录](review-fixes-2026-11-21.md)。

2026-11-23 05:24 十轮压力复验：`CHAST_NATIVE_MULTIWINDOW_CLOSE_CYCLES=10`，在一个真实 Flutter 进程中连续关闭/重连选中标签 10 次，兄弟连接保持活动，末尾精确断开通过。Release loopback 120.87 秒 PASS。轨迹 `build/native-runtime-20261123-052137-c14aa6cd/run-779652209/session-tabs.tsv` 有 10 次 `close-tab-request`/`close-tab-post`/`prune-child` 和 20 次 `viewer-bridge-close`，无窗口集归零或消息泵 quit/error；累计 46 个历史失败后的周期通过，旧故障未复现，不据此认定修复。验收轮数默认 3、测试环境变量范围 1–20。详情见[审核修复记录](review-fixes-2026-11-21.md)。

2026-11-23 05:04 复验补充：修正 runner trace 的空标签/宿主销毁事件记录时机后，在 `CHAST_NATIVE_TEST_LIFECYCLE_TRACE=1` 下再次运行 `tools/test-native-runtime.ps1 -BuildDirectory build\root -Flutter -MultiWindow`，Windows Release 构建及 loopback 通过，三轮关闭/重连、兄弟连接保持及末尾精确断开均通过。轨迹 `build/native-runtime-20261123-050322-6bd81ab4/run-1018272889/session-tabs.tsv` 每轮记录 `close-tab-request`、`close-tab-post`、`viewer-bridge-close` 和 `prune-child`；未出现窗口集归零、标签归零/宿主销毁或 `GetMessage` quit/error 事件。旧进程退出未复现；新增事件不能证明根因修复，关闭失败回滚仍未验收，关闭行为未改。详情见[审核修复记录](review-fixes-2026-11-21.md)。

2026-11-22 23:48 复验补充：启用详细生命周期 trace 连续三批运行 `-MultiWindow` Release loopback，每批三轮关闭/重连、兄弟连接保持和末尾管理台精确断开均通过。三份轨迹分别位于 `build/native-runtime-20261122-234512-64332027/run-4193874309/`、`build/native-runtime-20261122-234624-b6c7a031/run-406761616/`、`build/native-runtime-20261122-234730-8cc8cf40/run-4281782606/`，各记录 3 次 `close-tab-request`/`prune-child`、6 次 `viewer-bridge-close`。累计 27 个后续关闭周期通过。12:05/12:08 两次关闭后进程退出仍未定位，未复现不等于修复，关闭失败回滚仍未验收，关闭行为未改。

2026-11-22 23:41 复验补充：随后移除详细生命周期 trace，再运行 `-MultiWindow` Release loopback，连续三轮关闭/重连、兄弟连接保持及末尾管理台精确断开全部通过。最新 trace-disabled 证据 `build/native-runtime-20261122-234008-86a4c7b9/run-674030585/session-tabs.tsv` 每轮记录 `close-tab-request`、`close-tab-post`、`prune-child`；累计 18 个后续关闭周期通过。12:05/12:08 两次关闭后进程退出仍未定位，未复现不等于修复，关闭失败回滚仍未验收，关闭行为未改。

2026-11-22 23:28 复验补充：`-MultiWindow` Release loopback 在详细生命周期 trace 开启时再连续三轮关闭/重连全部通过，最新轨迹 `build/native-runtime-20261122-232717-55f31d69/run-8867425/session-tabs.tsv` 每轮均记录 child `WM_CLOSE`、Dart 确认、`WM_DESTROY`/`WM_NCDESTROY` 和 `prune-child`。累计 15 个后续关闭周期通过；前置认证失败没有进入关闭流程。12:05/12:08 两次关闭后进程退出仍未定位，未复现不等于修复，关闭失败回滚仍未验收，关闭行为未改。`CHAST_NATIVE_TEST_LIFECYCLE_TRACE=1` 可启用 runner/bridge 详细事件；关闭失败时 Go 验收附带查看端退出码。证据与边界见[审核修复记录](review-fixes-2026-11-21.md)。

2026-11-22 12:08 追加：标签关闭冲突已在当前 Release 构建复现。两次运行都到达 `close-tab-request`/`close-tab-post`，随后在 15 秒断言前进程退出；12:08 失败快照无该进程顶层窗口。回查归档 TSV 发现仅有标签层请求/投递记录，没有 runner 生命周期事件，故不能判断 child 后续销毁或消息泵状态，退出信号来源仍未知。仅增加 UI 失败诊断，标签关闭实现保持未改。

2026-11-22 11:53 追加：专用 Flutter Windows Release loopback 已通过合成双逻辑显示器下的子窗口父会话生命周期验收。A/B 子窗口随父标签切换；断开 A 后 A 子窗口关闭，B 子窗口和标签保持可用，native 状态确认 A ended、B streaming 且解码持续。两个逻辑显示器映射到同一只读本机桌面捕获，不更改 Windows 显示拓扑，不代表物理多屏/DPI/热插拔验收。C01 双标签 lease 首断/末断 loopback 也已通过，真实 OS 电源行为仍待。

状态：基础实现已进入 `codex/complete-workspace-2026-11-20` 工作区。2026-11-22 Release loopback 已验证同进程双会话切换、输入释放/后台拒绝、兄弟隐藏时持续收流与录制、逐标签录制按钮状态。09:51 原生 CTest 通过合成音频下双 ViewerEngine 静音/播放隔离。10:20 专用 Release Flutter fixture 实际点击标签音量按钮，并用独立原生状态快照证明静音只作用于目标会话、切回后状态保持且两路持续解码；合成源和丢弃型播放器未打开物理输出，连续 3/3 PASS。另新增 `SessionTabVisibility` 原生回归，覆盖标签切换时子窗口所有权/可见性及 HWND 重新分配；11:53 专用 Release loopback 验证合成双逻辑显示器下显示子窗口随父标签切换，断开 A 只关闭 A 子窗口且 B 继续 streaming/解码。C01 双标签首断/末断 lease loopback 已通过。07:59 和 08:04 两次关闭测试观察到标签/会话窗口计数归零，与 07:07 通过结果冲突；关闭行为当前不判定通过，也未按约束修改关闭逻辑。物理显示/电源及真实音频验收仍待。

标签展示规则：可显示设备名相同时，仅给冲突组追加公开 Host ID 短尾号；不能安全地区分时追加稳定会话序号。新增或关闭标签会重新计算冲突组，保留标签本身的进程内 ID；唯一设备名不显示冗余标识。任何凭据、token、连接密钥都不得用于标签文案。

当前覆盖范围：

- **已有实现**：稳定 ID 标签模型及关闭邻居选择；原生容器托管每标签独立 Flutter engine/宿主窗口；提供新建和关闭按钮；选择时切换子窗口可见性；切出时调用会话输入停用路径，切入时通知 Dart 恢复当前标签状态；同名设备标签使用经校验的 Host ID 短尾号或稳定会话序号消歧。
- **已验证**：标签模型原生单测；同一真实 Flutter 进程内两条 loopback 会话的创建和切换；08:31 回环验收证明切换输入释放、隐藏标签鼠标输入拒绝及兄弟会话隐藏时持续收流/解码/发布/录制；08:59 原生状态快照证明 A 不录制、B 正在录制且隐藏期间样本增长；09:29 录制按钮 A/B 视觉状态自动断言通过；09:51 原生双 ViewerEngine 合成音频测试通过静音状态与内存播放器输出块隔离；10:20 Release Flutter UI fixture 通过实际音量按钮操作证明静音只作用于选中会话，隐藏/切回后状态保持且两路持续解码；新增 `session_tab_visibility` 原生测试证明窗口归属、可见性切换、owner 重分配及无效 HWND 清理；11:53 Release loopback 证明合成双逻辑显示器下父子窗口切换/断开生命周期；C01 双标签首断/末断 lease loopback 通过，策略测试拒绝 ended/failed/idle 会话的迟到获取。
- **仍未验证/未完成**：标签关闭冲突曾在 12:05、12:08 两次运行中复现为 close request/post 后查看端进程退出；累计正常关闭/重连通过且 2026-11-24 的无效 HWND 失败注入证明该明确失败路径可回滚，但旧失败根因仍未定位，不能据此判定历史问题已修复；本次也未模拟消息队列满载。物理多显示器子窗口父连接关闭/重连、混合 DPI/旋转/全屏/热插拔及兄弟标签在真实硬件下不受影响仍待验证；物理电源行为、键盘导航/辅助技术行为仍待。合成静音音频及丢弃型播放器不证明真实音质、物理设备或语音/AEC。

## 用户行为与会话归属

查看端提供一个包含多个远端连接的窗口。新建连接增加标签，切换只改变可见连接，关闭和重连只作用于选中的连接。现有多显示器子窗口仍属于其父连接；关闭父连接必须结束其子窗口，不能影响其它连接。新建入口能回到设备列表，也保留独立窗口方式。

每条连接保留独立的 Flutter engine、`SharedViewerSession`、`ViewerEngine`、连接身份、策略快照、音频和录制状态。现有 Dart `ViewerBridge` 是静态 MethodChannel，在同一个 engine 内简单堆叠多个 `SessionPage` 会把控制发给同一个原生桥，不能作为实现方案。

优先使用原生标签容器托管独立的会话视图，复用现有每个连接独立 engine 的结构。标签是同一容器中的真实内容切换，不是已打开窗口列表或窗口置前按钮。标签名从连接的可显示设备名取得，不解析/展示 token、密码或账号密钥。

## 实现约束与未关闭验收点

1. 当前 `FlutterWindow` 的 `SetNextFrameCallback` 会显示窗口；被隐藏的标签首次出帧不能自行抢焦点或再次显示。
2. `ViewerBridge` 按窗口安装 subclass，并按当前窗口处理关闭、全屏、输入消息和文件对话框 owner。每个会话保持独立宿主 HWND，避免多个桥在同一 HWND 上用相同 subclass ID 互相覆盖。
3. `SharedViewerSession::SendInput/SendText` 当前要求 `GetForegroundWindow() == window`。托管子窗口后应验证所属顶层窗口为前台、当前会话视图可见且拥有输入焦点；仅改成同一个根窗口比较会使后台标签也能注入输入。
4. 标签切出先释放远端按键/鼠标、取消本地 IME 组合与相对鼠标锁定，并暂停该桥的键盘捕获。后台页面的轮询或迟到 `setKeyCapture` 不能重新抢走输入。切回由当前权限与页面焦点恢复捕获。
5. 当前窗口关闭先通知 Dart `windowCloseRequested`，完成停止录制、停止会话等清理后再销毁 engine。标签关闭沿用这条异步流程；容器关闭应等待全部标签结束，保持首个 VM 所属 engine 最后销毁。
6. 全屏针对容器生效，关闭/切换全屏标签有确定恢复行为；显示器子窗口仍保持各自全屏状态。标签布局与 `WM_DPICHANGED`、窗口最小尺寸、键盘导航一起验证。

## 实施与验收顺序

1. 提取标签集合和选中/关闭状态模型，使用稳定 ID；覆盖关闭后台标签、关闭当前标签、关闭最后标签、重复关闭及新建失败回滚。
2. 增加原生容器和会话视图托管，接入现有 `openSessionWindow`，保留独立窗口入口；标签提供设备名、关闭和新建操作及可访问名称。
3. 接入输入激活/释放和前台校验、全屏及异步关闭，补会话代次保护，不改变每个 ViewerEngine 的身份/媒体归属。
4. 原生回归证明切换先释放旧连接输入、后台标签拒绝输入、旧代次不能控制重连后的会话；Flutter 回归证明新建参数、失败回退、重连与关闭只作用于对应连接。
5. 真实 Release 运行两条隔离 loopback 连接：切换先释放旧连接输入且后台输入被拒；切换/关闭/重连 A 时 B 持续收流和录制；分别设置静音和录制，互不串用；关闭最后连接释放电源请求。多显示器子窗口绑定及父连接关闭另验。

物理混合 DPI、外部音频设备、双机/跨网验收另行记录。标签内部状态测试或两个独立窗口通过，均不能替代实际容器内切换验收。
