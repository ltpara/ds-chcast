# W1 当前状态、完成标记与后续计划

日期：2026-11-19

分支：`codex/s03-role-console-2026-11-17`

当前基线：`8b9e057`

适用范围：Windows 单机验收、W1 企业管理主干收尾。本文是 2026-11-19 的状态入口；历史文档中的中间结论如与本文冲突，以本文和对应原始证据为准。

## 1. 总体结论

W1 尚未完成，当前不能宣称发布候选通过。已经完成的部分集中在 W1 企业管理主干、单机原生进程、管理台断开、Flutter 视频/文件链路和临时 SYSTEM 服务监督验收；仍缺口主要是 Flutter 窗口真实键鼠释放序列、第二台 Windows 实机、跨网、正式 SCM 服务及生产环境。

当前证据等级分为三类：

- **已完成**：有自动回归或真实进程产物，能直接支撑对应结论。
- **单机已验证**：只在 loopback、本机模拟输入或临时服务身份下通过，不能替代双机、跨网或正式服务。
- **待验收**：代码或工具已经存在，但没有达到完成所需的真实环境证据。

## 2. 已完成项

| 项目 | 状态 | 证据与边界 |
| --- | --- | --- |
| S01 通用 Strategy | 已完成代码闭环，单机真实验证 | 服务端分配、签名投递、主机/查看端逐项回执、更新后下一会话采用新 revision、撤销后空策略已验证；证据 `build/native-runtime-20261118-222855-69bd0658/`。双机/跨网仍待验。 |
| S02 活动连接与精确断开 | 已完成代码闭环，单机真实验证 | 真实管理台页面按 connection ID 确认断开，目标会话产生签名 `confirmed_absent` 回执，非目标 Flutter 连接保持 active 且统计持续变化；边界仍为 loopback。 |
| S03 自定义 Admin Role | 已完成代码闭环，主要场景已验证 | 权限键、命名角色、global/group/object 绑定、权限上限、最后管理员保护、revision 并发保护已接通；SQLite/PostgreSQL 权限矩阵和主要浏览器场景已通过。 |
| F1–F4 审核缺陷 | 已完成代码修复 | 地址簿缓存覆盖、退出换号竞态、过期令牌录制归档、管理台换号刷新竞态已纳入回归；真实双客户端/慢网络/长录制仍有边界。 |
| R1–R5 S03 审核缺陷 | 已完成 | 账号提权、应急权限混用、设备删除权限错配、作用域入口/列表、机器令牌断开均已修复并有回归。 |
| Flutter 视频与文件链路 | 单机已完成 | 真实 Release Flutter 完成密码认证、首次指纹确认、HEVC/D3D11VA 收流；文件浏览器实际上传/下载 524325 字节并通过 SHA-256 核对。 |
| Flutter 双查看端与管理台断开 | 单机已完成 | 两个真实 Flutter 会话同时在线；管理台断开目标后，非目标连接保持 active，统计区在相邻样本中持续变化。 |
| 原生桌面流、输入映射与连接上限 | 单机已完成 | DXGI 1920×1080、MFX HEVC、D3D11VA 接收 39 帧；键盘回环探针通过；受管 3/4、本地 2/3 准入边界通过。 |
| 最低帧率下限 | 已确认 | `app/lib/widgets/remote_toolbar.dart` 的 `_minFps = 5`，本轮无需新增逻辑。 |
| 临时 SYSTEM 服务监督验收 | 单机已完成，范围有限 | SYSTEM 读取交互用户会话、登录用户身份子进程、UAC restricted token、重复轮询去重、异常退出 backoff、销毁监督不杀用户 manager、隐藏入口/无监听/不改普通用户设置等均 PASS；证据 `build/service-manager-startup-system.log`、`build/service-manager-entry-check.log`、`build/service-manager-entry-check.ps1`。 |
| Flutter 键鼠下行基础链路 | 部分完成 | 本轮 UI 控件定位已从固定坐标改为截图识别，并验证键盘按钮开启/关闭强调色状态；真实轨迹已出现鼠标 move/down/up 与 A down。尚不能据此宣称完整释放序列通过。 |

## 3. 2026-11-19 Flutter 键鼠验收结果

执行：

```powershell
.\tools\test-native-runtime.ps1 -Flutter -InputInjection
```

本轮真实产物：

```text
D:\ds-chcast\build\native-runtime-20261119-001020-ef1059e1\run-2160080674\
```

关键结果：

```json
{
  "control_on_clicked": true,
  "control_on_verified": true,
  "control_off_clicked": true,
  "control_off_verified": true,
  "mouse_events": 1,
  "a_down_events": 1,
  "remote_a_up_before_physical_cleanup": false,
  "physical_cleanup_has_no_key_a_event": false
}
```

输入轨迹确实出现：

```text
mouse move
mouse button_down
mouse button_up
key A down
```

随后在“关闭远程键鼠后等待 A up”处超时，且最终轨迹没有 A up。说明：

- 工具栏定位和控制开关状态验证已经修好，不能再把问题归因于“点错按钮”。
- 鼠标与 A down 已证明 Flutter 控制态能向主机下行输入。
- 关闭控制时远端 A 的释放序列仍未通过，属于当前明确的阻断项。
- 不能把本轮标为完整 Flutter 键鼠验收通过。

当前需要继续定位的是关闭控制后 Flutter `_releaseRemoteInput()` 到 `ViewerBridge.releaseInput()`/协议 `InputRelease`/主机 trace 的链路。只在输入记录中手工补一个断言不能算修复。

## 4. 已确认的设计与代码事实

- 最低 5 FPS 已存在，无需重复新增。
- 关闭控制逻辑在 `app/lib/pages/session_page.dart` 的 `_toggleControl()` 与 `_releaseRemoteInput()`：先释放本地记录的 `_heldKeys`，再释放鼠标，最后调用 `ViewerBridge.releaseInput()`。
- 原生输入采集在 `app/windows/viewer/keyboard_capture.cc`；物理 `SendInput` 的 `dwExtraInfo` 为 0，带测试注入标记的事件会被忽略。
- 主机 dry-run 的输入观察回调目前记录 `InputEvent`，而 `InputRelease` 是控制消息，不会直接进入现有 `input-events.jsonl` 的普通事件轨迹。因此验收必须区分“release 控制消息已经送达主机”和“主机注入器确实释放了 held key”，不能只看普通输入事件。
- 当前 `CHAST_NATIVE_TESTING=1`、`CHAST_NATIVE_INPUT_DRY_RUN=1`、`CHAST_NATIVE_INPUT_TRACE=...\input-events.jsonl` 配置正确，主机也确实接收到了 A down。

## 5. 后续计划

### P0：完成 Flutter 键鼠释放验收

1. 在主机侧增加可验证的输入释放证据：记录 `InputRelease` 控制消息、`InputInjector::release_held()` 调用和 dry-run `release_all()` 实际释放的 held key/mouse。测试模式专用，不影响生产行为。
2. 在 Flutter 验收结果中区分并验证：
   - 控制开启确实生效；
   - 鼠标和 A down 到达主机；
   - 关闭控制后主机收到 release barrier；
   - 远端 A 在物理清理 A up 前释放；
   - 物理清理 A up 不再产生第二条远端 A 事件。
3. 重新运行 `tools/test-native-runtime.ps1 -Flutter -InputInjection`，只有真实轨迹全部满足才把该项改为完成。
4. 如果 release barrier 已到主机但 `release_all()` 没有产生 held key 释放，继续检查 dry-run 注入器持有集合与 trace observer 的顺序；如果 barrier 未到主机，检查 Flutter 的 `_heldKeys`、`ViewerBridge.releaseInput()`、QUIC `InputRelease` 和主机控制面处理。

### P1：SYSTEM 服务范围收口

1. 将现有临时 SYSTEM 服务证据登记为“临时服务监督验收通过”，明确不是正式 `chast-service.exe --install`、`--run`、`--worker`。
2. 后续在可回滚的测试机验证正式 SCM 安装、服务身份、启动/停止、worker、安全桌面、锁屏和账号切换。
3. 正式服务验收前，不以临时服务日志替代发布门槛。

### P2：第二台 Windows 与跨网

1. 准备第二台可连接 Windows 实机；记录显示器数量、DPI/缩放、网络拓扑和连接方式，不记录密码。
2. 验证真实 Flutter 键鼠、跨机文件传输、双物理机管理台断开、断网/重连、跨 NAT/代理和较长故障窗口。
3. 单机 loopback 的成功结果保持为单机证据，不升级为双机或跨网结论。

### P3：进入 W2

W1 外部验收收口后，按现有计划优先进入 C01，再推进 C02–C04 与 H03 跨重启队列。账号隔离、越权、数据丢失、错误断开和核心连接阻断问题仍优先于功能扩展。

## 6. 当前门禁与提交前检查

本轮提交前应执行并记录：

```powershell
# Go
cd D:\ds-chcast\server
go test ./...
go vet ./...

# Flutter（在 D:\ds-chcast\app）
flutter analyze
flutter test

# 管理台 JavaScript
node --test server/internal/webadmin/app_session_test.cjs
node --check server/internal/webadmin/native_disconnect_browser.cjs

本轮实际门禁结果（2026-11-19 提交前复跑）：

| 门禁 | 结果 |
| --- | --- |
| `cd server; go test ./...` | PASS |
| `cd server; go vet ./...` | PASS |
| `cd app; flutter analyze` | PASS |
| `cd app; flutter test` | PASS，815/815 |
| `node --test server/internal/webadmin/app_session_test.cjs` | PASS，48/48 |
| `node --check server/internal/webadmin/native_disconnect_browser.cjs` | PASS |
| PowerShell Parser：`tools/native-flutter-input.ps1` | PASS |
| PowerShell Parser：`tools/native-flutter-file-transfer.ps1` | PASS |
| `git diff --check` | PASS |

# PowerShell 脚本
# 使用 PowerShell Parser 检查新增脚本

# 通用
git diff --check
```

未执行的 PostgreSQL 生产拓扑、SCM 正式服务、第二台 Windows、跨网和性能对照继续标记待验收，不能用单机结果替代。

## 7. 证据索引

- 本轮键鼠失败证据：`build/native-runtime-20261119-001020-ef1059e1/run-2160080674/`
- 2026-11-18 单机组合验收：`build/native-runtime-20261118-214914-882e8a81/`
- 2026-11-18 双 Flutter/文件/管理台断开：`build/native-runtime-20261118-215415-dbfc4d4a/run-369350478/`
- 2026-11-18 viewer 策略三阶段：`build/native-runtime-20261118-222855-69bd0658/`
- 临时 SYSTEM 服务：`build/service-manager-startup-system.log`
- 详细原生验收：[native-runtime-acceptance-2026-11-17.md](native-runtime-acceptance-2026-11-17.md)
