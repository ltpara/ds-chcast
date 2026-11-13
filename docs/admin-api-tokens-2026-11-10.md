# 管理权限颗粒度与自动化 API Token（G12，2026-11-10）

本轮落地 G12 的第一片：把控制台授权从单一角色扩展为“资源 × 动作 × 组范围”，并新增与账号会话分离的机器令牌（创建、到期、撤销、最近使用、审计）。对照仍为本地 RustDesk `7423dced37845c48093b6f71bd82e4a16cc8c79d` 的 Pro Admin Role / API Token 语义（S16/S17）：本实现不复制其代码或依赖，只对齐“独立凭据、可缩小范围、可撤销、有审计”的行为。第二片（2026-11-10）补齐了设备与录制的组范围过滤；第三片补齐 webadmin 的“API 令牌”页（创建、列表、撤销、组范围资源联动与一次性密钥对话框），并已在本地真实浏览器完成创建/撤销冒烟验证。

## 权限模型

### 角色阶梯

账号角色仍为 `readonly < operator < admin`，server key 为部署根凭据。机器令牌在此之上再加一层“资源集合”：

| 资源 | 覆盖的控制台面 | 角色下限 |
| --- | --- | --- |
| `devices` | `/v1/identities` 设备列表、注销/重置/改身份，设备策略 CRUD，账号组内设备的增删 | 读为 readonly，写为 operator，策略为 admin |
| `audit` | `/v1/audit`、`/v1/connections`、`/v1/file-transfers` 及其 CSV 导出 | readonly |
| `accounts` | `/v1/accounts` 用户管理、管理员密码重置、API Token 管理 | admin |
| `groups` | 账号组创建/修改/删除、成员与角色管理、组详情与成员列表 | 读为 readonly，成员写为 admin |
| `recordings` | `/v1/recordings` 列表、下载 | 读为 readonly，删除为 operator |
| `ops` | `/v1/stats` 运行统计 | readonly |
| `all` | 以上全部 | 按各自动作 |

资源名在写入时统一小写、去重、排序；“角色决定动作上限，资源决定可进入的面”，两者必须同时满足。server key 不接受资源裁剪，也不持有令牌（创建时返回 400）。

### 机器令牌

- 普通账号（含 admin）可为**自己**创建令牌：`POST /v1/api-tokens`，字段 `name`、`role`、`resources[]`、可选 `group_id`、可选 `expires_in_seconds`。默认有效期 90 天，允许 1 分钟至 365 天；每用户最多 20 个未撤销且未过期的令牌。
- 明文只返回一次，形如 `chast_api_<随机>`；数据库只存 SHA-256。列表和审计只含元数据。
- 每次请求都重新读取 owner：令牌角色 = min(创建时角色, owner 当前角色)，owner 被降级后令牌自动收窄；owner 被封禁立即 401；过期或撤销立即 401。最近使用时间按分钟节流写回。
- 撤销 `DELETE /v1/api-tokens/{id}` 立即生效；重复撤销幂等，未知 id 返回 404。
- 组范围覆盖 `groups`、`audit`、`devices` 与 `recordings`：组范围令牌的列表只返回该组；跨组详情/成员/审计一律 403，未带 `group` 参数的审计自动收窄到该组。设备范围按 `account_group_devices` 过滤 `/v1/identities` 与设备策略列表，单设备操作（注销、重置、改标签、设备策略读写、应急访问）先验证目标在组内，否则 403；全局策略对象/分配、`access-kids` 退役与列表不允许组范围令牌，统一 403。录制范围在列表、总量、下载和删除上生效：录制所有者是组成员或录制目标设备属于该组时才可见。`accounts`、`ops` 仍是全部署面，带组范围创建直接拒绝。

### 审计

创建与撤销分别写入 `api_token_create`、`api_token_revoke`，记录操作者、令牌 id/名称/角色/资源/组/到期时间；撤销只记令牌 id。令牌每次使用不逐条写审计，避免自动化请求淹没安全日志。

## HTTP API

| 方法与路径 | 行为 | 最低要求 |
| --- | --- | --- |
| `GET /v1/api-tokens` | 列出调用者自己的令牌（含过期/撤销行，便于解释失效） | admin + `accounts` |
| `POST /v1/api-tokens` | 创建令牌，返回一次明文 secret | admin + `accounts` |
| `DELETE /v1/api-tokens/{id}` | 撤销，204 | admin + `accounts` |

示例：

```http
POST /v1/api-tokens
Authorization: Bearer <admin session token>
Content-Type: application/json

{"name":"nightly-backup","role":"operator","resources":["recordings","devices"],"expires_in_seconds":2592000}
```

创建响应中的 `secret` 不再显示；之后用 `Authorization: Bearer chast_api_...` 调用被授权的控制台接口。

## 实现

- 存储：`server/internal/account/api_tokens.go:17` 定义前缀，`:132` 创建，`:225` 列表，`:279` 撤销，`:315` 认证；资源名校验、角色上限、组范围与配额都在同一层收口。组范围资源集合见 `tokenGroupScopedResources`。
- Schema：`server/internal/account/migrate.go:14` 升到 v11，`:33` 建 `api_tokens` 表及 `api_tokens_user`、`api_tokens_expiry` 索引；Postgres 同版本迁移见 `server/internal/account/migrate_postgres.go:29`。
- 认证分流：`server/internal/api/console.go:59` 的 `consolePrincipalFor` 按 `chast_api_` 前缀进入令牌校验，资源与角色同时检查；`deviceActor`/`deviceInScope` 对单设备操作执行组范围，`requireUnscoped` 拒绝全局管理；`server/internal/api/groups.go:116`、`:175`、`:232` 分别处理组动作、审计组过滤与组列表。
- 组范围数据：设备成员来自 `account_group_devices`（`DeviceInGroup`、`GroupDevices`）；`/v1/identities` 与设备策略列表按成员过滤，单设备操作、应急访问与策略读写逐一校验。录制范围由 `AllRecordings`/`RecordingTotals` 的组参数和 `RecordingInGroup` 收口：所有者是组成员或目标设备属于该组。
- 管理台：`server/internal/webadmin/assets/index.html` 新增“API 令牌”页（名称、角色、有效期、可选分组、资源勾选、一次性密钥对话框与列表/撤销），`app.js` 的 `loadTokens`/`tokenRow`/`createToken`/`showTokenSecret` 负责接线；选择分组时界面自动禁用并清除账号、运维与全部资源。`webadmin_test.go` 的 `TestConsoleExposesAPITokenManagement` 锁定页面接线与 CSP 约束。
- 路由：`server/internal/api/api.go:338`。处理器：`server/internal/api/api_tokens.go:92`（列表）、`:110`（创建）、`:142`（撤销）。
- 资源接入点：设备 `server/internal/api/identities.go:36`、`api.go:921`、`device_policies.go:83`；审计 `audit.go:29`、`connection.go:177`、`file.go:220`；账号 `accounts.go:73`；录制 `recordings_console.go:25/73/111`；统计 `api.go:1086`。
- 审计动作：`server/internal/account/audit.go:63-64`。

## 边界

- 机器令牌只走控制台 HTTP API，不替代查看器/主机会话凭据，也不参与连接 ACL 或票据签发；G02/G03 的连接准入仍由账号、设备策略和短期票据决定。
- 令牌管理界面只对“以账号登录且具有 admin 角色”的管理台会话可见；server key 会话看不到该页，因为令牌必须由账号持有。列表只展示元数据，明文 secret 没有再次读取入口。
- 组范围已覆盖账号组、组审计、设备与录制；`accounts`、`ops` 等全部署面仍不能带组范围，创建时直接拒绝。
- 令牌不能在服务端重启后恢复明文；遗失只能撤销重建。没有自动清理过期行的后台任务，过期行保留用于解释失效。
- Postgres 迁移语句已同步；2026-11-14 已在 PostgreSQL 13.23/14.25 真库通过账号/对等存储集成、v17→v18 迁移、版本间恢复和双实例冷启动。生产 PostgreSQL 部署升级仍未验收，证据见 [PostgreSQL 13/14 真库验收](postgres-acceptance-2026-11-14.md)。
- 录制范围以“所有者在组内或目标设备在组内”判定；跨组的单条录制在读取/删除时返回 403，并在所有者的审计里记录一次被拒删除。设备组范围只覆盖控制台设备面，不改变连接 ACL、票据签发或主机注册。

## 验证

| 层级 | 结果 |
| --- | --- |
| 已实现 | v11 schema（SQLite/Postgres）、令牌创建/列表/撤销/认证、资源裁剪、组范围（账号组/审计/设备/录制）、角色上限与降级收窄、封禁拒绝、配额、到期、最近使用、创建/撤销审计、控制台各面接入；设备与录制按组过滤，全局策略/签发状态拒绝组范围令牌；webadmin“API 令牌”页（仅 admin 账号可见） |
| 已构建 | `go build ./...` 通过（2026-11-10） |
| 自动化通过 | `go test ./...` 全绿、`go vet ./...` 无问题；`TestAPITokenLifecycle`、`TestAPITokenRejectsOutOfScopeRequests`、`TestAPITokenExpiryDemotionAndBan`、`TestAPITokenQuotaAndGroups`、`TestRecordingGroupScope` 覆盖存储层；`TestAPITokenScopesAndLifecycle`、`TestAPITokenExpiryAndOwnerDemotion`、`TestAPITokenDeviceGroupScope`、`TestConsoleRecordingsGroupScope` 经真实 HTTP 回环覆盖资源拒绝、组范围列表过滤、单设备越组 403、全局面拒绝、设备策略读写、跨组录制下载/删除拒绝、审计、撤销与过期；`TestConsoleExposesAPITokenManagement` 锁定管理台页面接线 |
| 真库集成通过 | PostgreSQL 13.23/14.25 账号/对等测试、v17→v18 迁移、13→14 dump/restore 与双实例冷启动通过；真实生产部署升级到 v11 和外部脚本长期运行令牌仍待验证 |
| 待人工验证 | 管理台令牌页已在本地真实浏览器冒烟（admin 账号可见并可创建/撤销、server key 不可见、分组范围禁用全局资源、一次性 secret 对话框、无控制台错误）；生产反向代理下的浏览器行为、真实运维脚本的令牌轮换流程、20 个配额边界操作、跨天到期与时钟偏差、审计导出中令牌事件的可读性仍待验证 |

自动化测试同时覆盖临时 SQLite、真实 PostgreSQL 13/14 和 HTTP 回环；真库结果不证明生产反向代理、托管 PostgreSQL 或外部调度系统的行为。
