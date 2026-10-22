# M7 安全模型 — 当前状态

更新时间：2026-10-22
分支基线：`main`（`ea35163`）之上的未提交工作，快照见 `feat/m7-security`。
计划文档：`docs/m7-plan.md`。

## 一句话状态

协议、策略层、主机接线、主机 UI 已完成并通过单元/回环与一次真实窗口
操作验证；查看器（Flutter + CLI 参数）与完整回环验收尚未开始。
本页记录已完成的范围、实测证据，以及运行中发现但尚未解决的三个问题。

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
  允许/拒绝。对话框用定时器观察 `quitting`/`server_stopping`，
  关停时自动拒绝，避免阻塞退出。
- 托盘菜单同步：复制设备 ID、权限开关、自动启动、安全设置、连接服务、
  打开设置文件、退出。

### 6. 设置迁移（`src/host/host_settings.cc`）

- 新增 `auth_mode`、`password_hash`、`token_auth`、`permissions`、
  `allow_permission_change`、`totp_secret`、`trusted_devices`、
  `ip_whitelist`；旧 `approve=`/`token=` 自动迁移。
- 读取时若同时存在 `permissions` 与旧的 `input/clipboard/audio`，
  以 `permissions` 为准（UI 写文件时两者都写）。

## 验证结果（2026-10-22）

- 构建：`cmake --build build --config Release` 通过；仅剩既有的
  `chast-service` LNK4098 与 `pwsh.exe` 缺失提示。
- 单元/回环：`ctest -C Release` **10/10** 通过
  （cursor、remote_cursor、protocol、security、host_settings、
  host_security、host_tunnel、bitrate、audio、quic_loopback）。
- `chast-quic verify`：全项通过，**151 access units / 151 pictures，
  digest `807D64A283BE475B`**；新增断言
  “password denial, approval mask and live permissions work”与
  “service mode demands a password and the temporary password rotates”。
- 主机 UI（本机隔离配置 `build/tray-test.ini`，bind 127.0.0.1:45443）：
  - 首页读到设备 ID `desktop-j5pnsut`、临时密码 `528066`；
    点击“刷新”后变为 `259177`，摘要在有/无永久密码时正确切换。
  - 安全设置对话框可打开，三个页签可切换，页面控件显示/隐藏正确。
  - 点击批准模式：“密码或批准”模式下空密码连接弹出审批；
    改为 `auth_mode=click` 后文案变为“点击批准模式：每次连接都需要确认”。
  - 权限收窄实测：在审批窗取消“剪贴板”“系统声音”后允许，主机日志为
    `the operator approved …; granting keyboard and pointer`，
    CLI 查看器完成握手并进入会话（1920×1080、mfxhw64、7824 kbps）。
  - 密码模式/服务模式的策略拒绝由 `chast-quic verify` 的 HostServer
    级探针覆盖（见 `tools/quic.cc`）。

## 运行中发现的问题（未解决）

1. **COMCTL32 崩溃（一次性）**：17:16 在自动化连续切换页签时
   `chast-host.exe` 崩溃，事件日志为
   `COMCTL32.dll +0x5f9dc`，`0xc000041d`（随后 `0xc0000005`）。
   重启后用相同的 PostMessage 点击连续切换 30+ 次（含三页往返）
   未能复现；怀疑与 `PrintWindow`/自动化点击和 Tab 控件通知处理的
   交互有关，需要抓 dump 或加入断言后再定位。发布前必须关闭。
2. **审批超时后的僵留对话框**：CLI 查看器握手超时为 15 s。若操作者
   在超时后仍点“允许”，主机日志会出现
   `announce: capture failed: QUIC StreamOpen(video) failed: 0x80004004`
   （E_ABORT），审批窗不会自行关闭。需要让审批请求携带会话存活状态，
   在对端消失时自动关闭并记录“请求已超时”，而不是让操作者批准一个
   已死的连接。
3. **首次审批未生效是超时而非缺陷**：第一次验证时对话框停留约 37 s，
   查看器早已放弃，因此批准后仅得到 StreamOpen 失败。属于用例 2 的
   同一竞态，排查时应先缩短交互间隔或延长查看器握手超时。

## 尚未开始

- #70 查看器：设备身份与密钥生成、密码/2FA 对话框、记住密码
  （DPAPI）、拒绝码文案、工具栏“只看/剪贴板/音频”、权限更新提示。
- #71 工具与回环：`serve/connect` 增加密码、2FA、模式、白名单参数；
  loopback 覆盖成功/错误密码/锁定/2FA/可信设备/审批/实时权限；
  审批超时与死连接用例。
- #72 Flutter 验证与文档：`flutter analyze/test/build windows --release`，
  M7 计划文档收尾，路线图更新。

## 复现

```bash
# 构建与测试
"/c/Program Files/CMake/bin/cmake.exe" --build build --config Release
"/c/Program Files/CMake/bin/ctest.exe" -C Release --output-on-failure
./build/Release/chast-quic.exe verify

# 隔离的主机实例（不影响真实 host.ini）
# build/tray-test.ini: bind=127.0.0.1, port=45443, audio=0, tunnel=0
./build/Release/chast-host.exe --config build/tray-test.ini

# 点击批准 / 密码 / 两者可在安全设置里切换，或直接改 auth_mode=
./build/Release/chast-quic.exe connect --port 45443 --insecure --seconds 8
```

## 注意

- 本快照包含此前未提交的 M6 弱网验收产物
  （`tools/netem.cc`、`docs/weak-network-acceptance.md`）与路线图更新，
  它们与 M7 无关，但同属当前工作树状态。
- 临时密码、`host.ini`、测试脚本与构建产物均在 `build/` 下，未入库。
