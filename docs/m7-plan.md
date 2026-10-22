# M7 — 安全模型：认证、审批与权限

状态：进行中（2026-10-22）
上一阶段：M6.1 分辨率自适应与单机弱网验收已完成。
参考：RustDesk 的安全能力与操作流程（只做行为对齐，不复制代码）。

## 目标

把“访问令牌 + 可选审批”升级成一套可产品化的安全模型：

1. **密码认证**：永久密码（用户设置）+ 临时密码（6 位，启动生成，
   使用/连续失败/手动刷新后轮换）。
2. **三种接入模式**：`password`（只认密码）、`click`（只弹审批）、
   `both`（有密码走密码，空密码走审批）。默认 `both`。
   输错密码不会静默回退到审批。
3. **TOTP 两步验证**：密码通过后要求 6 位动态码，SHA1/30 s/±1 窗口；
   开启时显示 base32 密钥与 `otpauth://` URI，输入验证码确认后生效。
4. **可信设备**：查看器持有 ECDSA P-256 设备密钥；2FA 成功后勾选
   “信任此设备”，下次携带签名跳过 2FA（密码仍然要输）。90 天过期，
   可在主机端撤销；修改永久密码清空全部可信设备。
5. **失败锁定**：按来源 IP 计数，1 分钟内 ≥6 次或 10 分钟内 ≥30 次
   触发指数退避（30 s 起，翻倍，上限 5 分钟）；成功即清零。
6. **IP 白名单**：IPv4 CIDR 列表，空表示不限制；在密码校验之前拒绝。
7. **权限协商**：键盘、剪贴板、音频三个权限位；查看器在 Hello 里
   声明请求，主机按默认权限求交集；审批窗可逐项勾选；会话中主机
   可实时收回/恢复，查看器本地另有“只看”开关。
8. **操作方式对齐**：主机首页展示 ID/临时密码/刷新；设置里有安全页；
   查看器连接时弹密码框（可记住密码）、2FA 框、等待审批；错误提示
   区分密码错误/2FA 错误/锁定/被拒/无密码。
9. **令牌降级**：access token 只保留给 CLI/自动化，默认关闭；Flutter
   查看器只用密码，不再展示令牌。服务模式强制永久密码，否则拒绝
   连接；临时密码和点击审批只在交互模式可用。

## 非目标

- ID 白名单：查看器没有稳定 ID；可信设备公钥是更强的身份，本轮先做
  IP 白名单。
- 二维码：首期显示 base32 与 otpauth URI，认证器手动添加；二维码
  后续补。
- 2FA 恢复码、Telegram 推送、账号体系、多查看者权限合并。
- 修改服务端注册的 TLS/密钥体系；服务端仍建议反代 TLS。

## 威胁模型与边界

- 传输已是 QUIC + TLS 1.3，主机证书自签、查看器 TOFU 固定指纹；
  密码、TOTP、设备签名都在 TLS 内，中继看不到。
- 本机管理员可读取配置/DPAPI，属于可信边界之外，不防。
- 临时密码只有 6 位，靠锁定 + 轮换兜底；永久密码要求 ≥8 字符。
- IP 白名单针对主机观察到的对端地址；经中继时该地址是中继地址，
  这是与 RustDesk 相同的限制，文档中明确。

## 操作流程

### 主机首次配置

1. 首页显示设备 ID 与临时密码（交互模式），可复制/刷新。
2. “安全设置”：
   - 设置永久密码（当前密码 + 新密码 + 确认），≥8 字符。
   - 选择接入模式：密码 / 点击批准 / 两者。
   - 启用 2FA：显示 base32 与 otpauth URI，输入 6 位码确认；关闭
     需当前密码。
   - 可信设备列表：显示名称/平台/最近使用，可撤销。
   - IP 白名单：每行一个 CIDR。
   - 默认权限：键盘/剪贴板/音频；是否允许审批时改权限。
   - 高级：允许 token 认证（CLI）。
3. 服务模式启动时检查永久密码；没有则拒绝服务并在日志说明。

### 查看器连接

1. 输入服务器与设备 ID，点连接。
2. 密码框：可勾“记住密码”（DPAPI 当前用户加密，按设备保存）；
   留空表示请求对方点击批准。
3. 密码错误重新弹框；被锁定显示剩余时间；主机拒绝显示被拒。
4. 主机要求 2FA：弹 6 位码输入框，可勾“信任此设备”；错误重新输入。
5. 主机为点击模式时显示“等待对方接受…”。
6. 进入会话后工具栏可切换“只看”、剪贴板、音频；主机收回权限时
   对应按钮禁用并提示。

### 主机审批

审批窗显示来源 IP、设备名称/平台、认证方式、请求的权限；每个权限
一个复选框，默认取主机默认权限与请求的交集。按钮“允许/拒绝”。
服务模式不弹审批。

## 协议 v13

Hello 在 v12 的 `version, display, token` 之后追加：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| password | u16 len + bytes | UTF-8，≤128 B |
| requested_permissions | u16 | bit0 键盘、bit1 剪贴板、bit2 音频 |
| device_key | u16 len + bytes | ECDSA P-256 公钥 blob，≤128 B |
| device_platform | u8 | 1 = Windows |
| device_name | u16 len + bytes | UTF-8，≤64 B |
| client_nonce | 16 B | 防重放 |
| client_unix_seconds | u64 | 时间戳，主机允许 ±120 s |
| auth_flags | u8 | bit0 请求信任此设备、bit1 带签名 |
| signature | u16 len + bytes | ECDSA-SHA256(host_fingerprint‖nonce‖time) |
| totp | u16 len + bytes | 6 位 ASCII，可空 |

- 签名绑定主机指纹和客户端 nonce/时间戳；主机缓存最近 5 分钟的
  nonce 防止重放。
- 2FA 采用“拒绝后重连带 TOTP”，不引入额外挑战消息；用户看到的
  仍是密码后弹 2FA。主机返回 `TwoFactorRequired` 时查看器弹框并
  重连。
- `AccessDenial` 追加：`WrongPassword`、`PasswordRequired`、
  `TwoFactorRequired`、`WrongTwoFactor`、`LockedOut`、
  `NotWhitelisted`、`AuthTimeout`。
- `HelloAck`/`StreamInfo` 末尾追加 u16 `permissions`；旧的
  `input_enabled/clipboard_enabled/audio.enabled` 由它派生。
- 新增 `PermissionUpdate` 控制消息（host → viewer，u16 掩码）。
- 协议版本升 13，ALPN 不变。

## 存储

- `host.ini` 新增：
  - `auth_mode=password|click|both`
  - `password_hash=pbkdf2-sha256$210000$<salt_hex>$<hash_hex>`
  - `token_auth=0|1`
  - `permissions=<mask>`（默认 7）
  - `allow_permission_change=0|1`
  - `totp_secret=<DPAPI base64>`（机器/当前用户作用域，按配置目录）
  - `trusted_devices=<base64(pubkey)>:<base64(name)>:<base64(platform)>:added:last_seen,...`
  - `ip_whitelist=<cidr>,<cidr>,...`
- 永久密码只存 PBKDF2 哈希；临时密码只在内存。
- 2FA 密钥用 DPAPI 加密；设备公钥不是秘密，明文存列表。
- 修改永久密码：清空可信设备；保留 2FA（旧密码不再能登录）。

## 实现分层

1. `src/security/`：CSPRNG、PBKDF2-HMAC-SHA256、恒定时间比较、
   base32、TOTP、DPAPI、ECDSA 设备身份；独立测试。
2. 协议 v13：Hello/HelloAck/AccessDenial/PermissionUpdate 编解码与测试。
3. `HostSecurity`：白名单、锁定、密码/临时密码、2FA、可信设备、
   权限策略；审批决策结构。
4. `QuicSession`：Hello 解析后调用认证回调，审批回调可返回权限，
   会话权限可更新并下发。
5. `HostServer`/worker/`host/main.cc`：接线、服务模式强制密码、
   安全设置对话框、审批对话框、实时权限开关、临时密码展示/刷新。
6. 查看器：设备身份、密码/2FA/信任参数、DPAPI 记住密码、错误映射、
   工具栏只看/剪贴板/音频、权限更新提示。
7. 工具与测试：`serve/connect` 增加密码/2FA/模式/白名单参数；
   loopback 覆盖成功/失败/锁定/2FA/可信设备/审批/权限；ctest、
   Flutter analyze/test/build。

## 验证标准

- 单元：PBKDF2 测试向量、TOTP RFC 6238 向量、锁定边界、CIDR、
  权限交集、可信设备签名与过期、协议截断/超长。
- 回环：正确密码成功；错误密码拒绝；空密码在 password 模式拒绝、
  在 both 模式进入审批；2FA 要求/通过/失败；可信设备跳过 2FA；
  审批勾选后权限生效；会话中收回输入后主机不再注入；token 默认
  关闭、显式开启可用。
- 主机：交互模式首页有临时密码且刷新生效；服务模式无密码拒绝连接；
  安全设置修改后新连接生效。
- 查看器：密码/2FA 对话框、记住密码、错误文案、只看开关、
  权限收回提示；`flutter analyze`、`flutter test`、`flutter build
  windows --release`。
- 手工：两台机器（或本机双进程）按上述操作流程走一遍。

## 已知限制

- IP 白名单在中继路径上看到的是中继地址。
- 暂无 ID 白名单、二维码、恢复码、多查看者。
- 服务模式不弹审批；必须预先设置永久密码。
