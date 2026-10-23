# M7 安全模型 — 当前状态

更新时间：2026-10-23
分支：`feat/m7-security`。本次更新关闭 #73（安全对话框加固与压测）与
#74（查看器认证 GUI 验收），M7 剩余工作清零。
计划文档：`docs/m7-plan.md`。

## 一句话状态

协议、策略层、主机接线、主机 UI、CLI 认证与查看器认证/权限 UI 均已验收；
安全对话框的 COMCTL32 崩溃路径已加固，三页签两轮各 300 次点击压测无异常、
无新增 dump；查看器密码/2FA/记住/信任/撤销与实时权限的 Release GUI 路径
全部手工走通。

## 已完成

### 1. 安全原语（`src/security/`、`include/chast/security.h`）

- CSPRNG 临时密码/令牌、PBKDF2-HMAC-SHA256（210k 次）、恒定时间比较。
- Base32 编解码、TOTP（SHA1 / 30 s / ±1 窗口）、`otpauth://` URI。
- DPAPI 当前用户作用域的 TOTP 密钥保护。
- ECDSA P-256 设备身份与 Hello 签名（绑定主机指纹 + nonce + 时间戳）。

### 2. 协议 v13（`include/chast/protocol.h`、`src/net/protocol.cc`）

- Hello 追加密码、请求权限、设备公钥/平台/名称、nonce、时间戳、
  认证标志、签名、TOTP；所有长度字段有上限，解析器拒绝截断与超长。
- `AccessDenial` 追加：`WrongPassword`、`PasswordRequired`、
  `TwoFactorRequired`、`WrongTwoFactor`、`LockedOut`、
  `NotWhitelisted`、`AuthTimeout`。
- `HelloAck`/`StreamInfo` 末尾追加 `permissions` 掩码；旧
  `input_enabled/clipboard_enabled/audio.enabled` 由它派生。
- 新增 `PermissionUpdate` 控制消息；版本升 13，ALPN 不变。

### 3. 策略层（`src/host/host_security.cc`）

决策顺序固定为：IP 白名单 → 服务模式缺密码拒绝 → 按地址锁定 →
显式令牌 → 点击/空密码审批 → 永久/临时密码 → TOTP（可信设备签名可跳过）。

- 失败锁定：1 分钟 ≥6 次或 10 分钟 ≥30 次触发 30 s 起、翻倍、上限
  5 分钟的退避；成功清零。
- 临时密码：交互式密码/两者模式生成，使用、连续失败、手动刷新后轮换；
  点击模式与服务模式不生成。
- 可信设备：90 天过期，可在主机撤销；修改永久密码清空列表。
- 权限：默认权限与 Hello 请求求交集，审批可再收窄。

### 4. 传输与会话（`src/net/quic.cc`、`src/host/host_server.cc`）

- Hello 解析后调用 `authenticate` 回调；需要审批时调用 `approve_client`，
  回调返回允许的权限掩码；`note_connected` 是唯一提交点。
- 会话中 `HostServer::set_permissions()` 排队，经帧循环应用并向查看器
  下发 `PermissionUpdate`；收回的权限立即作用于剪贴板、音频与输入注入。
- 服务模式没有永久密码时照常监听，但拒绝每个查看器并在日志说明原因；
  过期/错误密码、锁定、白名单各有明确拒绝码。
- 审批请求带 `ClientRequest::peer_alive`：回调返回后传输层再检查一次，
  查看者已离开则以 `AuthTimeout` 拒绝，不再向死连接 announce；`HostServer`
  同时放弃提交可信设备/已用临时密码。

### 5. 主机 UI（`host/main.cc`）

- 首页改为“设备 ID + 临时密码 + 安全摘要”：
  - 设备 ID（隧道 ID）与“复制 ID”；
  - 临时密码与“复制密码”“刷新”；
  - “接入：<模式> · 永久密码<已设置/未设置> · 2FA <已启用/关闭> ·
    默认权限：<…>”；
  - “本次会话权限：<…>”；
  - 键盘/剪贴板/声音开关：空闲时保存后重启监听，会话中实时下发。
- 安全设置对话框（620×566，三个页签）：
  - 认证：接入模式（密码/点击批准/两者）、修改永久密码（≥8 字符，
    修改时校验当前密码，修改后清空可信设备）、2FA 启用/关闭
    （启用显示 Base32 与 otpauth URI，输码确认后 DPAPI 加密保存；
    关闭需当前密码）。
  - 访问控制：可信设备列表与撤销、IP 白名单（逗号/换行、CIDR 归一化）、
    新连接默认权限、是否允许会话中调整权限。
  - 高级：访问令牌开关/一次性模式/复制/生成、身份指纹。
- 审批对话框（置顶、4000 号段控件）：来源 IP、设备名称与平台、
  认证方式、是否携带设备证明；三个权限复选框（无权限位时禁用），
  允许/拒绝。对话框用定时器观察 `quitting`/`server_stopping` 和
  `peer_alive`：关停时自动拒绝；查看者在审批期间离开时自动关闭并
  记录“查看者在审批期间离开”，既避免阻塞退出，也不会批准死连接。
- 托盘菜单同步：复制设备 ID、权限开关、自动启动、安全设置、连接服务、
  打开设置文件、退出。

### 6. 设置迁移（`src/host/host_settings.cc`）

- 新增 `auth_mode`、`password_hash`、`token_auth`、`permissions`、
  `allow_permission_change`、`totp_secret`、`trusted_devices`、
  `ip_whitelist`；旧 `approve=`/`token=` 自动迁移。
- 读取时若同时存在 `permissions` 与旧的 `input/clipboard/audio`，
  以 `permissions` 为准（UI 写文件时两者都写）。

### 7. 查看器认证与权限（#70）

- `app/windows/viewer/viewer_engine.*`：`Start` 接收密码、TOTP、设备身份
  与“信任此设备”；Hello 携带签名与信任标志。会话中应用
  `HelloAck`/`StreamInfo`/`PermissionUpdate` 的权限掩码，输入与剪贴板
  在传输层之前本地闸门；状态向 Dart 暴露 `permissions` 与
  `permissionUpdates`。
- `app/windows/viewer/viewer_bridge.*`：首次连接生成/加载
  `%APPDATA%\chast\viewer-identity.bin`（DPAPI）；`loadPassword`/
  `savePassword` 以 `%APPDATA%\chast\passwords\<FNV-1a64>.bin` +
  DPAPI 保存“记住密码”，空密码删除文件；超过长度上限的凭据在桥接层拒绝。
- `app/lib/viewer_bridge.dart`：转发密码/2FA/信任参数，新增密码存取、
  权限位、1–10 号拒绝码的中文文案与 `permissions/permissionUpdates` 解析。
- `app/lib/pages/session_page.dart`：连接前自动读取已保存密码；密码/
  2FA 拒绝不再落到最终错误卡片，而是弹出对应对话框（密码可勾选记住，
  2FA 可勾选信任此设备）；取消后回到可重试的错误卡片；会话中权限变化
  以 SnackBar 说明；新增本地剪贴板同步开关，与既有的只看/键鼠/音频
  开关并列。
- `app/lib/widgets/remote_toolbar.dart`：新增剪贴板同步按钮，服务端
  未开放时禁用。
- 构建接线：查看器静态 FFmpeg 的编解码注册表会拉入 Opus 对象，
  `CMakeLists.txt` 补链 `opus.lib`；runner 补 `shell32.lib`/`ole32.lib`
  以支持 DPAPI 与已知文件夹路径。

### 8. 回环补充（#71）

`tools/quic.cc` 的 `verify` 新增 `secure_auth_wiring_works`，全部经过
真实 QUIC 连接：

- HostServer 的 `settings.totp_secret` 路径：仅密码 → `TwoFactorRequired`；
  错误动态码 → `WrongTwoFactor`；`settings.ip_whitelist` 不包含本机 →
  `NotWhitelisted`。
- 传输层 + `HostSecurity`：携带设备身份、正确密码、动态码并请求信任的
  查看者被准入，主机记住该设备；重连时同一设备只凭正确密码跳过动态码；
  同一设备输错密码仍 `WrongPassword`；陌生设备无动态码仍被要求 2FA。
- 同一来源连续 6 次错误密码后，正确的永久密码也被 `LockedOut` 拒绝，
  理由为 “too many failed attempts”。

### 9. 安全对话框加固（#73）

- `security_dialog_proc` 的 `WM_NOTIFY` 只接受 `hwndFrom == dialog->tab`、
  `idFrom == kSecTab`、`code == TCN_SELCHANGE` 的通知；`lparam` 为空或
  来源不匹配直接忽略，避免对伪造/迟到通知解引用。
- `TCN_SELCHANGE` 不再在 COMCTL32 回调内直接 `ShowWindow` 页面控件，而是
  `PostMessage(WM_APP+7)` 延迟到通知栈展开后再读取 `TabCtrl_GetCurSel`
  切换；延迟期间页签再次变化时以最新选中项为准。
- WER LocalDumps 按可执行文件配置：
  `HKCU\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\chast-host.exe`
  → `DumpFolder=D:\ds-chcast\build\dumps`、`DumpType=2`（完整）、
  `DumpCount=5`。

### 10. 查看器认证 GUI 验收（#74）

Release 查看器 + 隔离主机（`build/tray-test.ini`，仅绑定 127.0.0.1）
逐项走通：密码错误、密码正确、记住密码重连、2FA 错误、2FA 正确并信任
设备、可信设备重连跳过动态码、主机撤销信任后重新要求动态码，以及会话中
收回输入/剪贴板/音频时工具栏与 SnackBar 同步。

## 验证结果（2026-10-22）

- 构建：`cmake --build build --config Release` 通过；仅剩既有的
  `chast-service` LNK4098 与 `pwsh.exe` 缺失提示。
- 单元/回环：`ctest --test-dir build -C Release` **10/10** 通过
  （cursor、remote_cursor、protocol、security、host_settings、
  host_security、host_tunnel、bitrate、audio、quic_loopback；
  其中 quic_loopback 27.07 s）。
- `chast-quic verify`：**83 项断言全过**，**151 access units / 151
  pictures**，1920×1080 编码（源 2560×1440）、2 次流更新，
  CRC/解码/序号/协议错误为 0。新增断言
  “two-factor, trusted devices, lockout and the allow-list work”
  覆盖上述 #71 用例；审批存活断言仍通过。digest 每次随桌面内容变化，
  不作为固定值比较。
- Flutter 查看器（#72）：
  - `flutter analyze` 无问题；
  - `flutter test` **62 项通过**（含拒绝码 4–10、权限位解析、
    显示器菜单与工具栏构造）；
  - `flutter build windows --release` 成功
    （`build\windows\x64\runner\Release\chast_viewer.exe`）。
  - 新密码/2FA 对话框的鼠标点击路径尚未自动化，见“剩余工作”。
- CLI 密码/2FA 端到端：
  - `serve --password secret123` + `connect --password secret123`：
    主机日志 `admitted 127.0.0.1:54766 with a password`；查看者
    181 帧、零 CRC/解码/序号/协议错误，digest `AF2C3F3B344D905C`。
  - 同一主机使用错误密码：`handshake: permission denied:
    incorrect password`，主机日志 `refused …: incorrect password`，
    监听器继续等待下一位。
  - `serve --password secret123 --totp-secret JBSWY3DPEHPK3PXP` +
    `connect` 相同参数：准入并解码 241 帧，零错误。
  - 用另一个 base32 密钥生成错误动态码：`permission denied:
    incorrect two-factor code`，主机日志同码。
- 主机 UI（本机隔离配置 `build/tray-test.ini` / `tray-test2.ini`，
  bind 127.0.0.1）：
  - 首页读到设备 ID `desktop-j5pnsut`、临时密码 `528066`；
    点击“刷新”后变为 `259177`，摘要在有/无永久密码时正确切换。
  - 安全设置对话框可打开，三个页签可切换，页面控件显示/隐藏正确。
  - 点击批准模式：“密码或批准”模式下空密码连接弹出审批；
    改为 `auth_mode=click` 后文案变为“点击批准模式：每次连接都需要确认”。
  - 权限收窄实测：在审批窗取消“剪贴板”“系统声音”后允许，主机日志为
    `the operator approved …; granting keyboard and pointer`，
    CLI 查看器完成握手并进入会话（1920×1080、mfxhw64、7824 kbps）。
  - 审批超时（端口 45455）：CLI 查看者 15 s 握手超时断开后，审批窗在
    200 ms 轮询内自动关闭；日志为
    `查看者在审批期间离开，已取消来自 127.0.0.1:50980 的请求`，
    不再出现 `QUIC StreamOpen(video) … 0x80004004`。
  - 密码模式/服务模式的策略拒绝由 `chast-quic verify` 的 HostServer
    级探针覆盖（见 `tools/quic.cc`）。

## 验证结果（2026-10-23）

- #73 页签压测（Release，真实窗口，每次点击后 `PrintWindow` 全窗截图）：
  - 常规：100 轮 × 3 页往返 = **300 次点击**，mismatch=0、slow=0、
    elapsed 14.7 s，主机存活、`build/dumps` 无新增。
  - 快速：100 轮 × 3 页，点击间隔 5 ms 并叠加 `PrintWindow`，
    **300 次点击**，mismatch=0，final page=2 正确，elapsed 13.1 s，
    主机存活、无新增 dump。
  - 页面切换以每个页面代表控件的 `IsWindowVisible` 校验
    （5004 / 5014 / 5021）。
- #74 GUI 验收（Release `chast_viewer.exe --connect 127.0.0.1:45443`）：
  - 密码错误：主机日志 `refused …: incorrect password`，对话框显示
    “密码不正确，重试。”。
  - 密码正确：`admitted 127.0.0.1:60366 with a password`（09:39:07），
    记住密码写入 DPAPI 存储。
  - 记住密码重启：无对话框直接 `admitted … with a password`（09:40:11）。
  - 2FA 错误：主机 `refused …: incorrect two-factor code`（09:58:34），
    查看器重新弹出“输入两步验证码”，提示“两步验证码不正确或已过期，
    请重新输入。”。
  - 2FA 正确 + 勾选“信任此设备”：`admitted … with a password`（09:59:24），
    进入 1920×1080 / mfxhw64 / 7824 kbps 会话。
  - 重启查看器：无 2FA 提示，`admitted … with a trusted device`（09:59:51）。
  - 主机在“访问控制”页撤销信任后保存（页面切换走的就是新的延迟
    `TCN_SELCHANGE` 路径）：`trusted_devices=` 清空、共享重启（10:01:25）；
    查看器重连被 `refused …: two-factor authentication is required`
    （10:01:34）并再次弹出验证码对话框。
- 自动化输入说明：2FA 码必须恰好为 6 位 ASCII 数字；不足 6 位会在 Hello
  解析阶段被 `malformed TOTP code` 拒绝（协议层兜底），属于测试脚本问题，
  不是产品缺陷。

## 已解决的问题

- **COMCTL32 页签崩溃（一次性）**：17:16 自动化连续切换页签时
  `chast-host.exe` 崩溃，事件日志为 `COMCTL32.dll +0x5f9dc`、
  `0xc000041d`（随后 `0xc0000005`）。现在 `WM_NOTIFY` 校验通知来源，并把
  `TCN_SELCHANGE` 的页面切换 `PostMessage` 到通知栈外执行；两轮各 300 次
  点击（含 5 ms 快速点击 + `PrintWindow`）均无 mismatch、无崩溃、无新增
  dump。原崩溃 dump 保留在
  `C:\Users\admin\AppData\Local\CrashDumps\`（`chast-host.exe.11744.dmp`
  与 `chast-host.exe(1).11744.dmp`），可用 WER 新配置的
  `build/dumps` 复现时留档。
- **审批超时后的僵留对话框**：原先查看者握手超时断开后，审批窗仍可被
  点击“允许”，随后 `announce` 向已死的连接开流并以 E_ABORT 失败。
  现在 `ClientRequest::peer_alive` 由传输层注入，审批回调返回后传输层
  再检查一次，死者以 `AuthTimeout` 拒绝；`HostServer` 不提交可信设备与
  已用临时密码；主机审批窗按 200 ms 轮询自动关闭并记录原因。回环断言与
  真实窗口测试均通过（见上节）。
- **首次审批未生效是超时而非缺陷**：第一次验证时对话框停留约 37 s，
  查看器早已放弃；该现象与上一条是同一竞态，修复后不再出现。

## 仍未解决

无。

## 剩余工作

- M7 无剩余验收项。#73 的 `host/main.cc` 改动与本文档一并提交；
  测试脚本与 dump 目录留在本地 `build/`，不入库。

## 复现

```bash
# 构建与测试
"/c/Program Files/CMake/bin/cmake.exe" --build build --config Release
"/c/Program Files/CMake/bin/ctest.exe" --test-dir build -C Release --output-on-failure
./build/Release/chast-quic.exe verify

# Flutter 查看器
cd app
flutter analyze
flutter test
flutter build windows --release

# CLI 密码 / 2FA（serve 服务一位查看者后退出）
./build/Release/chast-quic.exe serve --port 45456 --password secret123
./build/Release/chast-quic.exe connect --port 45456 --insecure --password secret123

./build/Release/chast-quic.exe serve --port 45458 --password secret123 --totp-secret JBSWY3DPEHPK3PXP
./build/Release/chast-quic.exe connect --port 45458 --insecure --password secret123 --totp-secret JBSWY3DPEHPK3PXP

# 隔离的主机实例（不影响真实 host.ini）
# build/tray-test.ini: bind=127.0.0.1, port=45443, audio=0, tunnel=0
./build/Release/chast-host.exe --config build/tray-test.ini

# 点击批准 / 密码 / 两者可在安全设置里切换，或直接改 auth_mode=
./build/Release/chast-quic.exe connect --port 45443 --insecure --seconds 8

# 安全对话框页签压测（先打开“安全设置”对话框）
# 每轮依次点击 认证 / 访问控制 / 高级，点击后 PrintWindow 全窗截图，
# 以各页代表控件的可见性核对页面切换；-Rapid 为 5 ms 间隔快速点击。
powershell -NoProfile -ExecutionPolicy Bypass -File build/tabstress.ps1 -Rounds 100
powershell -NoProfile -ExecutionPolicy Bypass -File build/tabstress.ps1 -Rounds 100 -Rapid

# 查看器认证 GUI（Release，配合隔离主机）
./app/build/windows/x64/runner/Release/chast_viewer.exe --connect 127.0.0.1:45443
```

## 注意

- `b7ce199`（WIP 快照）已包含 M6 弱网验收产物
  （`tools/netem.cc`、`docs/weak-network-acceptance.md`）与路线图更新；
  M7 的查看器 UI、回环与文档收尾在其之上完成。
- #73 只改 `host/main.cc`（`WM_NOTIFY` 校验与延迟页面切换），无协议或
  策略层变化。
- 临时密码、`host.ini`、测试脚本、dump 与构建产物均在 `build/` 下，未入库。
