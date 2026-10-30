# M11 P2-19：WebAuthn 安全密钥（chast 自有设计）

状态：完成（2026-10-28，含同日补齐的免密码登录）。
目标：账号可以用 Windows Hello / FIDO2 安全密钥完成两步验证登录或免密码
登录，并在账号对话框里注册、命名、删除安全密钥。只改 HTTP 账号 API，不改
会话协议（保持 v27），查看器的原生桥直接调用 Windows `webauthn.dll`。

## 1. 与 RustDesk 的差异与取舍

RustDesk 的两步验证只有 TOTP 与恢复码，没有 WebAuthn；这项是 chast 账号
安全的补齐，不是逐行对照。

- 安全密钥是**登录方式的补充**：默认流程仍是密码 + 断言；账号也可以选择
  免密码登录（用户名选中本账号已注册的凭据后直接断言）。没有密钥的账号
  行为不变。
- 不做浏览器自助页：查看器通过 Windows Hello 平台认证器完成注册与断言，
  服务端只暴露 JSON API，任何浏览器/原生客户端都能实现同样流程。
- 免密码登录是**用户名作用域**的：必须先输入用户名来选出该账号的凭据，
  不做无用户名的可发现凭据（passkey）流程——查看器原生桥拒绝空 allow-list，
  匿名挑战还需要新的数据库结构与反枚举策略。不做跨设备同步
  （iCloud/Google 密钥环）：凭据只存在本机认证器，服务端只存公钥。
- 证明（attestation）接受 `none` 与 `packed`（自证明/带证书链但**不校验
  链信任**）。安全模型依赖注册时的密码认证，不依赖厂商证明；文档写明。

## 2. 接口（HTTP，协议版本不变）

配置：`--webauthn-rp-id`（裸主机名，例如 `rd.example.com`）与
`--webauthn-origin`（逗号分隔的 `scheme://host[:port]` 白名单）。两者缺一
即关闭安全密钥路由（503）。校验规则：每个 origin 的主机必须等于 rp-id 或
是其子域；clientDataJSON 的 origin 必须与白名单**逐字**匹配。

| 方法 | 路径 | 认证 | 说明 |
| --- | --- | --- | --- |
| POST | `/v1/account/webauthn/register/begin` | Bearer + 密码 | 校验密码后签发注册挑战，返回创建选项与现有凭据排除列表 |
| POST | `/v1/account/webauthn/register/finish` | Bearer | 校验 attestationObject 并保存公钥，返回凭据视图 |
| GET | `/v1/account/webauthn/credentials` | Bearer | 列出自己的安全密钥（ID、名称、创建/最近使用时间） |
| POST | `/v1/account/webauthn/credentials/remove` | Bearer + 密码 | 删除一枚安全密钥 |
| POST | `/v1/account/login/webauthn/begin` | 无（用户名+密码） | 密码正确且存在凭据时返回断言选项；无凭据返回 202，不签发挑战 |
| POST | `/v1/account/login/webauthn/finish` | 无 | 校验断言，原子消费密码登录挑战并签发令牌（同 `/v1/account/login`） |
| POST | `/v1/account/login/webauthn/passwordless/begin` | 无（用户名） | 账号存在且有凭据时返回断言选项；未知账号与无凭据统一 202，不签发挑战也不计限流失败 |
| POST | `/v1/account/login/webauthn/passwordless/finish` | 无 | 校验断言，原子消费免密码挑战并签发令牌 |

- 密码版登录开始时先过现有的 `LoginLimiter`（用户名+IP），密码错误按正常
  登录计数；封禁账号返回 403。注册要求密码 + 有效访问令牌。
- 免密码 begin 对未知账号与无凭据都返回 202 `{"status":"accepted"}` 且不记
  限流失败：既防账号枚举，也避免把无密钥账号误锁。封禁账号仍会拿到挑战，
  只有密钥持有者提交有效断言后由完成阶段返回 403，未认证探测看不到封禁状态。
  已限流的用户名+IP 在 begin 阶段直接 429。
- 挑战 32 字节随机、5 分钟过期、`(user_id, purpose)` 单活；`purpose` 为
  `register`、`login` 或 `login-passwordless`，两种登录挑战可同时存在，
  互不替换也互不重放。登录成功时在签发令牌的同一事务里删除挑战，重放必然
  失败。注册失败保留挑战，同一挑战可在 5 分钟内重试（断言签名无法被暴力枚举）。
- 断言签名计数按凭据保存，非零且回退时拒绝；成功登录更新 `last_used_at`。
- 审计：`webauthn registered` / `webauthn removed` / `login webauthn failed` /
  `login webauthn`（detail 为 `password+security-key device=...` 或
  `passwordless-security-key device=...`）。
- 联动：管理员 `user reset-2fa` 与账号删除清空凭据与挑战；封禁保留凭据但
  登录一律 403；改密不删除安全密钥（凭据是长期因素，与一次性验证码不同）；
  删除单枚凭据会同时清掉该账号两种登录挑战。

## 3. 存储

```sql
CREATE TABLE webauthn_credentials (
  credential_id BLOB PRIMARY KEY,
  user_id       TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  public_key    BLOB NOT NULL,   -- COSE_Key 原文
  alg           INTEGER NOT NULL,
  sign_count    INTEGER NOT NULL DEFAULT 0,
  name          TEXT NOT NULL,
  created_at    INTEGER NOT NULL,
  last_used_at  INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE webauthn_challenges (
  user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  purpose    TEXT NOT NULL,      -- register | login | login-passwordless
  challenge  BLOB NOT NULL,
  expires_at INTEGER NOT NULL,
  PRIMARY KEY (user_id, purpose)
);
```

## 4. 校验实现（`internal/webauthn`）

- 极简 CBOR 解码（定长 uint/负整数/字节串/文本/数组/映射，拒绝不定长），够
  解析 attestationObject、COSE_Key 与 packed 证明。
- COSE：ES256（EC2 P-256，alg -7）、RS256（RSA，alg -257）、EdDSA
  （OKP Ed25519，alg -8）。
- clientDataJSON：type 必须为 `webauthn.create` / `webauthn.get`，challenge
  与签发值逐字相等，origin 命中白名单，`crossOrigin` 必须为假。
- authData：rpIdHash = SHA-256(rp-id)，UP 必须置位，UV 记录但不强制；注册
  解析 AAGUID/凭据 ID/COSE 公钥，断言校验 `signature` 覆盖
  `authData || SHA-256(clientDataJSON)`。
- `packed`：无 `x5c` 时用新凭据公钥自验证；有 `x5c` 时用叶子证书公钥验证，
  不校验证书链（见取舍）。

## 5. 查看器

- 原生桥 `chast/viewer` 新增 `securityKeyAvailable` /
  `securityKeyMakeCredential` / `securityKeyGetAssertion`，内部调用
  `webauthn.dll`（Windows Hello），Dart 侧只传 JSON 字段与 base64url。
- 账号对话框新增“安全密钥”区块：列出、注册（要求密码）、删除（要求密码）。
- 登录对话框：两步验证提示下新增“使用安全密钥”，密码 + 断言完成后保存会话。
- 主登录表单新增“使用安全密钥登录”，只输入用户名即可发起免密码断言。

## 6. 验收

- `internal/webauthn` 软件认证器单测（ES256/RS256/EdDSA、origin/rpId/
  challenge/UP/计数器/格式错误）。
- `internal/account` 与 `internal/api` 覆盖注册、列表、删除、密码版与免密码
  登录、双 purpose 隔离、202、限流、封禁、重放与审计。
- 真实进程回环：Go 软件认证器客户端完成注册 → 列表 → 密码 + 断言登录 →
  免密码断言登录 → 重放拒绝 → 错误签名拒绝 → 删除。
- Flutter：`server_api_test`/`home_page_test` 覆盖请求、错误映射与无硬件
  时的降级提示；`flutter analyze`/`flutter test`/Release 构建。
- 限制：真实 Windows Hello 弹窗需要已配置 PIN/生物识别的机器人工验收；本机
  没有安全密钥硬件，回环用软件认证器。

## 7. 验收结果（2026-10-28）

- 服务端：`go vet ./...` 与 `go test ./... -count=1` 全通过，覆盖
  `internal/webauthn`（ES256/RS256/EdDSA、origin/rpId/challenge/UP/计数器/
  格式错误）与 `internal/api`（注册、列表、删除、登录、202、限流、封禁、
  重放与审计）。
- 真实进程回环：新增 `server/cmd/chast-webauthn-smoke`，连接真实
  `chast-server`（隔离 `state.db`、`--webauthn-rp-id 127.0.0.1`、
  `--webauthn-origin http://127.0.0.1:18443`）与回环管理台，用
  `internal/webauthn/webauthntest` 软件认证器完成 18 项检查，全部通过：
  管理台建号 → 密码登录 → 错误密码拒绝注册 → 注册选项 → 保存凭据 →
  列表 → 第二次注册的排除列表 → 登录 begin → 断言登录并签发访问/刷新令牌
  → 令牌可用 → 重放 401 → 篡改签名 401 → 失败后挑战仍可用 → 错误密码拒绝
  删除 → 删除 204 → 列表清空 → 无凭据 begin 返回 202 → 审计包含
  `webauthn_register`/`webauthn_remove`/`password+security-key`。
- 免密码扩展（同日）：探针增至 25 项并全部通过，新增免密码 begin 提供
  已注册凭据 → 断言完成并签发访问/刷新令牌 → 令牌可用 → 重放 401 → 未知
  账号 begin 202 → 删除凭据后免密码 begin 202；审计同时包含
  `password+security-key` 与 `passwordless-security-key device=...`。
  `internal/account` 覆盖双 purpose 并存、跨 purpose 拒绝与删除凭据清挑战，
  `internal/api` 覆盖跨 purpose 401、封禁 403 与限流 429。
- 查看器：`flutter analyze` 无问题，全量 388 项 Flutter 测试通过，
  `flutter build windows --release` 生成 `chast_viewer.exe`；新增
  `webauthn_api_test`（含免密码 begin/finish）、`security_key_test`
  （含 `loginWithSecurityKeyOnly`）、`security_key_dialog_test` 与
  `home_page_test` 两种安全密钥登录用例。
- 人工验收：真实 Windows Hello 弹窗要求机器已配置 PIN/生物识别，当前测试机
  不具备，保留为人工项；未配置平台认证器时对话框显示“此设备未配置 Windows
  Hello”，注册按钮仍可尝试，失败按原生错误码提示。
- 边界：attestation 只接受 `none` 与 `packed` 且不校验证书链（安全模型依赖
  注册时的密码认证）；免密码登录必须输入用户名（不支持无用户名的可发现
  凭据），不支持跨设备同步。

复现：

```bash
cd server
go build -o /tmp/chast-server ./cmd/chast-server
go build -o /tmp/chast-webauthn-smoke ./cmd/chast-webauthn-smoke
/tmp/chast-server --http-listen 127.0.0.1:18443 --punch-listen 127.0.0.1:18444 \
  --carrier-listen 127.0.0.1:18445 --admin-listen 127.0.0.1:18446 \
  --relay-listen 127.0.0.1 --relay-ports 41400-41409 --state /tmp/state.db \
  --identity /tmp/identity.key --webauthn-rp-id 127.0.0.1 \
  --webauthn-origin http://127.0.0.1:18443
/tmp/chast-webauthn-smoke --server http://127.0.0.1:18443 --admin 127.0.0.1:18446
```
