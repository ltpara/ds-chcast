# PostgreSQL 13/14 真库验收（2026-11-14）

## 1. 结论

2026-11-14 使用本机临时 PostgreSQL 13.23 与 14.25 实例，完成了 chast 服务端
账号/对等存储的真实数据库集成、v17 到 v18 应用迁移、13 到 14 的逻辑备份恢复，
以及两个服务端实例对空库的并发冷启动验收。

结论：PostgreSQL 后端、双份 schema 迁移和当前 schema v18/v3 的多实例初始化
已通过真库验证；PostgreSQL 14.25 上的 ACL 真主机整链也已通过本机复验。仍不能
把本次结果等同为生产 PostgreSQL 部署、生产反向代理、真实多主机网络、双机
Flutter 或高可用/故障切换验收。

## 2. 环境

| 项目 | PostgreSQL 13 | PostgreSQL 14 |
| --- | --- | --- |
| 服务端版本 | 13.23 | 14.25 |
| 地址 | `127.0.0.1:55413` | `127.0.0.1:55414` |
| 测试库 | `chast_test` | `chast_test`、`chast_cold` |
| 临时认证 | 本地 `trust` | 本地 `trust` |
| 数据目录 | 仓库外临时目录 | 仓库外临时目录 |

两个实例只监听回环地址，用于验收后即停止并清理。数据库、构建后的
`chast-server.exe`、密钥、日志和错误输出均位于仓库外临时目录，不进入仓库。

## 3. 集成与迁移测试

两个版本分别执行：

```powershell
$env:CHAST_TEST_POSTGRES_DSN = 'postgres://postgres@127.0.0.1:<port>/chast_test?sslmode=disable'
go test ./internal/account ./internal/registry -count=1 -run Postgres -v
```

PostgreSQL 13.23 和 14.25 均通过以下 6 项真库测试：

- `TestPostgresStore`
- `TestPostgresConcurrentLoginLocks`
- `TestPostgresSchemaMigrationBackfillsSessionActivity`
- `TestPostgresDevicePolicyReceipt`
- `TestPostgresAccountGroups`
- `internal/registry` 的 `TestPostgresStore`

新增的迁移测试先创建账号并登录，再移除 `user_tokens.login_at`/
`last_seen_at` 和 `schema_migrations` 的 v18 记录，随后重开 Store。两个版本都能
重新应用 v18，把两列从 `created_at` 回填，并通过 `SessionsPage` 读回一致的登录
时间与最近活动时间。

## 4. 版本间恢复

使用 PostgreSQL 13 的 `pg_dump -Fc` 导出 `chast_test`，再用 PostgreSQL 14 的
`pg_restore --exit-on-error` 恢复到 14.25。恢复后检查：

| 存储 | 当前最高版本 | 迁移记录数 |
| --- | ---: | ---: |
| 账号库 `schema_migrations` | v18 | 18 |
| 对等库 `registry_schema_migrations` | v3 | 3 |

因此 13 到 14 的恢复保留 schema 版本和完整迁移历史，恢复后的 14.25 实例同时通过
上节全部真库测试。

## 5. 双实例冷启动

在 PostgreSQL 14.25 的空库 `chast_cold` 上同时启动两个真实 `chast-server.exe`，
分别监听 HTTP `127.0.0.1:18943` 和 `127.0.0.1:18953`：

- 两个实例都进入 ready，日志均报告状态存储在 PostgreSQL；
- 两个 `/v1/server` 都返回 `accounts: true`；
- 账号迁移表恰有 18 条记录，最高 v18，v18 记录只有 1 条；
- 对等迁移表恰有 3 条记录，最高 v3，v3 记录只有 1 条；
- 没有重复建表、重复迁移或版本竞争错误。

这验证了空库上的 advisory lock 串行化与迁移记录幂等；不等同于生产负载下的滚动
升级、多版本二进制混跑或故障切换。

## 6. 发现并修复的 PostgreSQL 缺陷

首次真库测试在录像配额 SQL 上失败：

```text
ERROR: operator is not unique: unknown - unknown (SQLSTATE 42725)
```

原因是 `AddRecordingWithQuota` 的占位参数在 PostgreSQL 中保持未定类型，表达式
无法唯一解析。修复后对配额运算参数使用显式 `CAST(... AS BIGINT)`，SQLite 与
PostgreSQL 继续共用同一 SQL 路径；SQLite 的整数亲和性语义不变。

同时修正 PostgreSQL 账号组测试的清理顺序，先删除普通成员账号再删除管理员，
避免最后一名管理员保护把清理步骤误报为失败。

修复后服务端完整回归通过：

```text
go test ./... -count=1
go vet ./...
go build ./...
```

## 7. PostgreSQL 14.25 ACL 真主机整链复验

本次在同一 PostgreSQL 14.25 实例上创建 `chast_acl_20261114`，使用真实
`chast-server.exe`、真实 `chast-host.exe`、真实隧道和真实 QUIC 客户端重跑
ACL 整链。拓扑为全部端点位于本机回环，主机 QUIC 为
`127.0.0.1:25433`，服务端 HTTP/punch/carrier/admin 分别为
`28443/28444/28445/28446`。

| 场景 | 实际结果 | 结论 |
| --- | --- | --- |
| 权限 527→3 | 续租后主机报告 `permissions updated to 0x00000003`；收窄前文件列表成功，收窄后返回 `PermissionDenied`；861 帧，CRC/解码/gap/协议错误均为 0，持续 55.046 秒 | 通过 |
| 策略 ceiling 527→3，v4 | 存量会话 15.536 秒结束；223 帧，全部错误计数为 0；PostgreSQL 回执 v4 `applied=true` | 通过 |
| 策略恢复 527，v5 | PostgreSQL 回执 v5 `applied=true` | 通过 |
| 账号封禁 | 旧 token 换新票 HTTP 401；会话 59.741 秒结束；1763 帧，全部错误计数为 0；随后成功解封 | 通过 |
| 设备禁用 | 新票和 lookup 均 HTTP 403；会话 58.711 秒结束；1734 帧，全部错误计数为 0 | 通过 |
| 设备恢复 | `disabled=false`，lookup `online=true`，v5 票据恢复 | 通过 |
| 账号组授权 | 临时组 `acl-e2e-members`，策略 v6 只允许组；真实会话 59.718 秒，1764 帧，全部错误计数为 0 | 通过 |
| 移组撤权 | DELETE 成员 204；新票 HTTP 403 `not allowed by device policy`；存量会话按上述 59.718 秒结束 | 通过 |
| 组恢复 | 恢复用户策略 v7；临时组删除 204 | 通过 |
| kid v1 退役 | `POST /v1/access-kids` 成功并持久化；新票 HTTP 503；会话 15.800 秒结束；446 帧，全部错误计数为 0 | 通过 |
| v2 恢复 | 服务端以 `kid=acl-e2e-v2` 重启；lookup 在线；v2 票据；策略 v7 回执 true；真实会话 50.031 秒，1473 帧，全部错误计数为 0 | 通过 |

本轮同时发现并修复了 PostgreSQL 布尔类型缺陷：
`device_policy_receipts.applied` 在 PostgreSQL 中是 `BOOLEAN`，代码却沿用 SQLite
整数绑定和扫描。现在 `server/internal/account/device_policy.go` 使用 Go `bool`
直接绑定/扫描，`users.banned` 也按 `bool` 扫描；
`TestPostgresDevicePolicyReceipt` 覆盖回执 true/false、允许账号和封禁账号。

修复后 `go test ./internal/account -count=1`、`go vet ./...` 和
`go build ./...` 通过；上述 6 项 PostgreSQL 定向测试全部通过。测试工具在有限
观察窗口结束时可能返回 1 并报告 `stream did not end cleanly`，该状态不作为 ACL
失败依据，通过依据为准入结果、实际帧数、错误计数和撤权时间。本轮没有完成全量
`go test ./... -count=1`，不将其列为证据。

最终删除临时管理员 `acladmin`，仅保留 `alice`，管理员、未封禁、session cap 0；
临时组已删除，策略为 v7，服务端以 `kid=acl-e2e-v2` 运行。

## 8. 证据边界与后续项

- 本次没有在生产 PostgreSQL、生产反向代理/中继或真实多主机网络上运行。
- ACL 的账号封禁、设备禁用/恢复、移组撤权、权限收窄和 kid 退役已在本地
  PostgreSQL 14.25 上重跑通过；SQLite 单机回环结果仍见
  [ACL 真主机单机回环验收](access-control-acceptance-2026-11-13.md)。
- 本次 ACL 拓扑仍全部位于本机回环，使用单 PostgreSQL 实例、单显示器和测试
  QUIC 客户端；生产 PostgreSQL、真实反向代理/中继、双机 Flutter、真实新私钥
  轮换和 HA 尚未验收。
- 尚未验证生产库权限配置、托管数据库的 ICU 排序规则可用性、连接池容量、
  TLS 证书、备份保留、主从切换、升级回滚和长时间运行。
- 升级仍只支持前向迁移；降级应从备份恢复，不能依赖 down migration。

后续生产验收应在真实部署拓扑上重跑服务端启动、账号/组/策略操作、ACL 撤权、
备份恢复和双实例滚动升级，并记录数据库与代理版本、权限、连接参数和监控指标。
