# M9 — 输入与连接补全计划

更新时间：2026-10-24
分支：`feat/m7-security`
基线：M8.6 完成（协议 v19；锁屏/UAC 桌面跟随、隐私模式、回环
`verify --privacy`）。M9.0 将协议升到 v20。
参考：`D:\rustdesk` @ `c7f212d`（只做能力对照，不复制实现）

## 1. 范围与取舍

M8.6 之后，普通指针/键盘、剪贴板、文件、音频、隐私与锁屏路径都已打通。
剩下的功能缺口按使用频率排序，M9.0 只做前两项中最小、最影响日常操作的一组：

| 能力 | chast 现状 | RustDesk | 本阶段 |
| --- | --- | --- | --- |
| Ctrl+Alt+Del | 无法发送；`SendInput` 被系统忽略 | 服务态调用 `SendSAS` | M9.0 |
| 系统组合键 | 只能靠查看器本地键盘逐个按 | 工具栏/快捷键菜单 | M9.0 |
| 断线恢复 | 只能手动点“重新连接” | 自动重连 | M9.0 |
| 画质/编码器/FPS 设置 | 仅控制端 ABR，查看器无选择 | 多项 | M9.1 |
| 多文件/文件夹传输、CF_HDROP | 单文件；拖放只取第一项 | 支持 | M9.2 |
| 多显示器合并视图 | 一次只显示一块屏 | 支持 | M9.3 |
| 音频 AEC/噪声抑制/设备选择 | 仅有回声消除外的原始采集 | 支持 | M9.4 |

M9.0 不引入低级键盘钩子（`WH_KEYBOARD_LL`）：查看器能截获的组合键先用
工具栏显式发送；本地系统组合键（如 Ctrl+Alt+Del 被查看器操作系统截走）
本来也到不了 Flutter。后续如果需要“物理按键直通”再单独评估。

## 2. 协议 v20：SAS

`Ctrl+Alt+Del` 不是普通按键：Windows 只接受 `SendSAS(FALSE)` 产生的安全注意
序列，且调用方必须是 LocalSystem（服务工作进程）或被 `SoftwareSASGeneration`
策略放行的进程。查看器不能把它拆成 E0/E2/E4 三个 `InputEvent`。

版本从 19 升到 20，ALPN 保持 `chast/1`。新增两条控制消息：

| 类型 | 方向 | 载荷 |
| --- | --- | --- |
| `SendSas = 28` | 查看器 → 主机 | 空 |
| `SasResult = 29` | 主机 → 查看器 | `u8 code`、`u16 reason_len`、UTF-8 reason（≤256 B） |

结果码：

| code | 名称 | 含义 |
| --- | --- | --- |
| 0 | Ok | `SendSAS` 已调用 |
| 1 | Denied | 本次会话没有输入权限 |
| 2 | Unsupported | 托盘主机、旧系统或找不到 `sas.dll` |
| 3 | Failed | 平台调用失败，`reason` 说明原因 |

权限与信任边界：

- 会话层不丢弃无输入权限的 `SendSas`，而是交给主机回答 `Denied`。隐私模式
  请求采用“无权限即丢弃”，SAS 采用“明确拒绝”，因为用户按下后必须得到反馈。
- 主机在执行前重新检查 `kPermissionInput`；权限在排队期间被收回时同样拒绝。
- 只有 `HostServerConfig::service_mode` 的进程（`host/worker.cc`）会调用
  平台 `SendSAS`；托盘主机配置为 `false`，固定回答 `Unsupported` 并提示安装
  服务。测试可注入 `HostServerConfig::send_sas` 回调，不触碰真实桌面。
- `SendSAS` 没有返回值，主机只能确认调用发生。真实是否弹出安全桌面由 Windows
  决定，验收时必须人工锁屏验证。

## 3. 主机：服务态 SAS

- 新增 `include/chast/sas.h` / `src/win/sas.cc`：动态加载 `sas.dll` 的
  `SendSAS`，避免链接期依赖；`sas_available()` 报告导出是否存在。
- `HostServerConfig` 增加 `std::function<bool(std::string*)> send_sas`。为空时：
  `service_mode` 为真走平台调用，否则回答 `Unsupported`。
- 会话循环在主循环里与隐私请求同层抽取 `take_sas_request()`，每个请求恰好
  回答一次 `SasResult`；统计增加 `sas_requests` 与 `sas_failures`。
- 失败不结束会话：SAS 是独立动作，拒绝或平台失败只回到查看器。

## 4. 查看器：特殊键与 SAS

工具栏新增“特殊按键”菜单，全部通过现有 `InputEvent` 发送（按下再抬起，
每条事件带当前修饰键位图，不改变本机键盘状态）：

| 菜单项 | HID usage |
| --- | --- |
| Win | 0xE3 |
| Alt+Tab | 0xE2 + 0x2B |
| Alt+F4 | 0xE2 + 0x3D |
| Ctrl+Shift+Esc | 0xE0 + 0xE1 + 0x29 |
| Ctrl+Alt+Del | 单独走 `SendSas` |

规则：

- 菜单仅在远程控制开启且主机可用时启用；发送后立即抬起，避免主机残留按键。
- SAS 每个会话按次请求；`ViewerStatus` 增加 `sas_code`、`sas_error`、
  `sas_updates`。页面用计数配对回答，成功/拒绝各弹一次提示。
- 主机回答 `Denied`/`Unsupported` 时提示原因；传输断开则不作假成功。

## 5. 断线自动重连

只在“已经出过画面、随后意外断开”时自动重试，避免把密码错误、令牌拒绝或
证书指纹变化也当成网络抖动：

- 触发条件：`state == failed`、已见首帧、无接入拒绝、无指纹变化、页面未关闭。
- 退避：1 s、2 s、4 s、8 s、15 s，最多 5 次；重连复用同一隧道进程、访问
  令牌、密码/TOTP 与显示器选择。
- 等待期间显示“连接中断，正在重连（第 n/5 次）”，保留“取消连接”；首帧到达
  后计数清零。
- 手动“重新连接”和显示器/分辨率切换仍会立即重置自动重试计数。
- 隧道进程已退出时由 `_ensureTunnel()` 重建；隧道服务本身不可用则按启动失败
  处理，不无限重试。

## 6. 验证计划

- `chast-protocol-test`：`SendSas` 空载荷往返、尾随字节拒绝；`SasResult`
  四种 code、未知 code、超长 reason 截断、坏 UTF-8、空载荷。
- `chast-quic verify --sas`：主回环注入回调回答 `Ok`，断言查看器收到结果且
  主机只调用一次；无回调的托盘式主机断言 `Unsupported`。真实 `SendSAS` 由
  服务态人工锁屏验证。
- Flutter：特殊键事件序列（修饰键先按后放、顺序与位图）、SAS 状态解析与提示、
  自动重连退避/取消/成功后计数清零；`flutter analyze`、`flutter test`。
- Release 构建 + `ctest -C Release`；文档记录实际输出。
- 人工：服务安装后锁屏状态下发送 Ctrl+Alt+Del，确认出现安全桌面；托盘态确认
  收到 `Unsupported` 提示。

## 7. 不在本阶段

- 软件编码/GDI 回退、跨平台、KCP/TCP 主路径。
- 低级键盘钩子与物理组合键直通。
- 画质/编码器/FPS 选择（M9.1）、多文件与文件夹（M9.2）、合并多屏（M9.3）、
  音频 AEC 与设备选择（M9.4）。
- MSI/自动更新/i18n（M8.7 暂缓）。

## 8. 验证记录（2026-10-24，单机回环）

- `chast-protocol-test`：`SendSas` 空载荷往返与尾随字节拒绝；`SasResult`
  四种结果码、未知码、超长 reason 截断和坏/空载荷，全部通过。
- `chast-quic verify --sas`：96 项断言全过，0 失败；主回环主机用内存回调在
  1.62 s 回答 `Ok`，查看器收到 `code 0`，主机计数恰好 1 次请求、0 次失败；
  第二个无回调、非服务态的主机对请求回答 `Unsupported` 并附原因，且视频继续。
  本轮 131 个访问单元、摘要 `C895EAB372C09B59`。
- Flutter：`flutter analyze` 无问题；85 项测试通过，覆盖组合键按下/抬起顺序
  与修饰键位图、SAS 状态解析与提示映射、自动重连退避/取消/计数保持，以及
  特殊键菜单的可用性与选择回调。
- 原生：Release 全量构建通过；`ctest --test-dir build -C Release` 12/12
  （含 31 s 的 `quic_loopback`）。
- 查看器：`flutter build windows --release` 成功。
- 自动重连由纯策略模块 `app/lib/reconnect_policy.dart` 与页面状态机实现：
  `state == failed` 且已见首帧、无拒绝、无指纹变化时按 1/2/4/8/15 s 退避，
  最多 5 次；首帧、手动重连和显示器/分辨率切换都会清零。单机无法制造真实
  断网，退避与取消逻辑由单元测试覆盖。
- 仍待人工：服务安装后在锁屏状态发送 Ctrl+Alt+Del，确认安全桌面出现；托盘
  主机确认收到 `Unsupported` 文案；真实断网/恢复观察自动重连节奏。

## 9. 阶段进展

- M9.1（协议 v21）：查看器画质、帧率与编码器偏好，见
  `docs/m9.1-plan.md`，已完成。
- M9.2（协议 v22）：多文件、文件夹与 CF_HDROP 拖放，见
  `docs/m9.2-plan.md`，已完成。
- M9.3（协议 v23）：多显示器合并视图，见 `docs/m9.3-plan.md`，已完成。
- M9.4（协议仍 v23）：音频端点选择、回声消除与降噪，见
  `docs/m9.4-plan.md`，已完成。
