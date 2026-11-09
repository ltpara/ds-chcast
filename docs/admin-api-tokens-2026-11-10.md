# 管理权限颗粒度与自动化 API Token（G12，2026-11-10）

本轮落地 G12 的第一片：把控制台授权从单一角色扩展为“资源 × 动作 × 组范围”，并新增与账号会话分离的机器令牌（创建、到期、撤销、最近使用、审计）。对照仍为本地 RustDesk `7423dced37845c48093b6f71bd82e4a16cc8c79d` 的 Pro Admin Role / API Token 语义（S16/S17）：本实现不复制其代码或依赖，只对齐“独立凭据、可缩小范围、可撤销、有审计”的行为。

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
- 组范围只允许覆盖 `groups` 与 `audit`：列表只返回该组，跨组详情/成员/审计一律 403，未带 `group` 参数的审计自动收窄到该组。**设备资源暂不允许组范围**：设备列表和单设备操作尚无按组过滤，创建时直接拒绝，避免范围看似生效而实际覆盖全部设备。

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

- 存储：`server/internal/account/api_tokens.go:17` 定义前缀，`:132` 创建，`:225` 列表，`:279` 撤销，`:315` 认证；资源名校验、角色上限、组范围与配额都在同一层收口。
- Schema：`server/internal/account/migrate.go:14` 升到 v11，`:33` 建 `api_tokens` 表及 `api_tokens_user`、`api_tokens_expiry` 索引；Postgres 同版本迁移见 `server/internal/account/migrate_postgres.go:29`。
- 认证分流：`server/internal/api/console.go:59` 的 `consolePrincipalFor` 按 `chast_api_` 前缀进入令牌校验，资源与角色同时检查；`server/internal/api/groups.go:116`、`:175`、`:232` 分别处理组动作、审计组过滤与组列表。
- 路由：`server/internal/api/api.go:338`。处理器：`server/internal/api/api_tokens.go:92`（列表）、`:110`（创建）、`:142`（撤销）。
- 资源接入点：设备 `server/internal/api/identities.go:36`、`api.go:921`、`device_policies.go:83`；审计 `audit.go:29`、`connection.go:177`、`file.go:220`；账号 `accounts.go:73`；录制 `recordings_console.go:25/73/111`；统计 `api.go:1086`。
- 审计动作：`server/internal/account/audit.go:63-64`。

## 边界

- 机器令牌只走控制台 HTTP API，不替代查看器/主机会话凭据，也不参与连接 ACL 或票据签发；G02/G03 的连接准入仍由账号、设备策略和短期票据决定。
- 没有 webadmin 令牌管理界面；创建/撤销目前通过 HTTP API 完成。管理台仍按原有角色展示，不读取令牌资源矩阵。
- 组范围只覆盖账号组与组审计；设备按组过滤、录制按组过滤未实现。设备令牌必须不带 `group_id`。
- 令牌不能在服务端重启后恢复明文；遗失只能撤销重建。没有自动清理过期行的后台任务，过期行保留用于解释失效。
- Postgres 迁移语句已同步，但本机未连接真实 Postgres 验证；SQLite 迁移与升级路径由自动化覆盖。

## 验证

| 层级 | 结果 |
| --- | --- |
| 已实现 | v11 schema（SQLite/Postgres）、令牌创建/列表/撤销/认证、资源裁剪、组范围、角色上限与降级收窄、封禁拒绝、配额、到期、最近使用、创建/撤销审计、控制台各面接入 |
| 已构建 | `go build ./...` 通过（2026-11-10） |
| 自动化通过 | `go test ./...` 全绿、`go vet ./...` 无问题；`TestAPITokenLifecycle`、`TestAPITokenRejectsOutOfScopeRequests`、`TestAPITokenExpiryDemotionAndBan`、`TestAPITokenQuotaAndGroups` 覆盖存储层；`TestAPITokenScopesAndLifecycle`、`TestAPITokenExpiryAndOwnerDemotion` 经真实 HTTP 回环覆盖资源拒绝、组范围 403、列表过滤、server key 拒绝、越权创建、组范围创建拒绝、审计、撤销与过期 |
| 真实链路通过 | 待人工验证：真实部署升级到 v11、Postgres 实例、用外部脚本长期运行令牌 |
| 待人工验证 | 真实运维脚本的令牌轮换流程、20 个配额边界操作、跨天到期与时钟偏差、审计导出中令牌事件的可读性 |

自动化测试使用临时 SQLite 数据库和 HTTP 回环，不证明真实反向代理、Postgres 或外部调度系统的行为。
