# 状态检查点：PostgreSQL ACL 真主机复验（2026-11-14）

## 1. 当前结论

本轮在本机 PostgreSQL 14.25 真库上完成了 ACL 真主机整链复验。真实
`chast-server.exe`、真实 `chast-host.exe`、真实隧道和真实 QUIC 客户端均参与，
覆盖权限收窄、策略撤权、账号封禁、设备禁用/恢复、账号组授权/移组、kid 退役/
恢复。

结论：ACL 主链在 SQLite 单机回环和 PostgreSQL 14.25 本机回环上均已通过；ACL
总体仍为 **部分完成**。生产 PostgreSQL、真实反向代理/中继、双机 Flutter、真实
新私钥轮换和 HA 尚未验收。

## 2. 代码与文档基线

| 项目 | 状态 |
| --- | --- |
| 分支 | `main` |
| 本轮提交前 GitHub 基线 | `5590d75`（协议 v73） |
| 当前工作区协议 | v76 |
| 账号 schema | v18 |
| 本轮 ACL 数据库 | PostgreSQL 14.25，`chast_acl_20261114` |
| 最终票据 | `kid=acl-e2e-v2`，`policy_version=7` |

工作区仍有大量其他未提交功能。本轮提交只包含 PostgreSQL ACL 验收文档、状态
检查点和必要的 PostgreSQL 布尔类型修复，不包含其他功能改动。

## 3. 验收环境

| 项目 | 本次环境 |
| --- | --- |
| PostgreSQL | 14.25，PID 11040，`127.0.0.1:55414` |
| 服务端 | 当前工作区构建的真实 `chast-server.exe`，PostgreSQL 后端 |
| 主机 | 真实 `chast-host.exe`，PID 4456；配套隧道 PID 8 |
| 主机 QUIC | `127.0.0.1:25433` |
| 服务端端点 | HTTP `127.0.0.1:28443`、punch `28444`、carrier `28445`、admin `28446` |
| 会话客户端 | 真实 `chast-quic connect --access-ticket`，实际 DXGI 捕获、H.264 编码与解码 |
| 网络 | 全部端点位于本机回环，不代表 NAT、公网或生产拓扑 |

测试库、构建产物、密钥、日志和错误输出均位于仓库外临时目录，不进入仓库。

## 4. 验收结果

| 场景 | 结果 |
| --- | --- |
| 权限 527→3 | 续租后主机报告 `permissions updated to 0x00000003`；收窄前文件列表成功，收窄后 `PermissionDenied`；861 帧，CRC/解码/gap/协议错误全 0，持续 55.046 秒 |
| 策略 ceiling 527→3，v4 | 存量会话 15.536 秒结束；223 帧，全部错误计数为 0；PostgreSQL 回执 v4 `applied=true` |
| 策略恢复 527，v5 | PostgreSQL 回执 v5 `applied=true` |
| 账号封禁 | 旧 token 换新票 HTTP 401；会话 59.741 秒结束；1763 帧，全部错误计数为 0；随后成功解封 |
| 设备禁用 | 新票和 lookup 均 HTTP 403；会话 58.711 秒结束；1734 帧，全部错误计数为 0 |
| 设备恢复 | `disabled=false`，lookup `online=true`，v5 票据恢复 |
| 账号组授权 | 临时组 `acl-e2e-members`，策略 v6 只允许组；真实会话 59.718 秒，1764 帧，全部错误计数为 0 |
| 移组撤权 | DELETE 成员 204；新票 HTTP 403 `not allowed by device policy`；存量会话按上述 59.718 秒结束 |
| 组恢复 | 恢复用户策略 v7；临时组删除 204 |
| kid v1 退役 | `POST /v1/access-kids` 成功并持久化；新票 HTTP 503；会话 15.800 秒结束；446 帧，全部错误计数为 0 |
| v2 恢复 | 服务端以 `kid=acl-e2e-v2` 重启；lookup 在线；v2 票据；策略 v7 回执 true；真实会话 50.031 秒，1473 帧，全部错误计数为 0 |

测试工具在有限观察窗口结束时可能返回 1 并报告
`stream did not end cleanly`；该退出状态来自观察窗口，不作为 ACL 失败依据。
通过依据是准入结果、实际帧数、错误计数和撤权时间。

## 5. 本轮修复

真库复验发现 `device_policy_receipts.applied` 在 PostgreSQL 中是 `BOOLEAN`，
而代码按 SQLite 的整数语义绑定和扫描，导致回执写入/读取失败。修复内容：

- `server/internal/account/device_policy.go`：`applied` 改为 Go `bool`，INSERT/UPDATE
  直接绑定布尔值；`users.banned` 改为扫描 `bool`，判断改为 `case banned:`。
- `server/internal/account/postgres_test.go`：新增 `TestPostgresDevicePolicyReceipt`，
  覆盖回执 true/false、允许账号和封禁账号。

修复后通过：

```text
go test ./internal/account -count=1
go vet ./...
go build ./...
```

真实 PostgreSQL 定向测试通过：

```text
TestPostgresStore
TestPostgresConcurrentLoginLocks
TestPostgresSchemaMigrationBackfillsSessionActivity
TestPostgresDevicePolicyReceipt
TestPostgresAccountGroups
internal/registry TestPostgresStore
```

本轮未完成全量 `go test ./... -count=1`，因此不把全量测试作为本次证据。

## 6. 最终状态

- 临时管理员 `acladmin` 已删除；最终只有 `alice`，管理员、未封禁、session cap 0。
- 临时组 `acl-e2e-members` 已删除，用户策略恢复为 v7。
- 服务端以 PostgreSQL 后端和 `kid=acl-e2e-v2` 运行；lookup 在线，v2 票据可签发。
- SQLite 单机回环结果见
  [ACL 真主机单机回环验收](access-control-acceptance-2026-11-13.md)。
- PostgreSQL 存储、迁移、恢复和 ACL 整链结果见
  [PostgreSQL 13/14 真库验收](postgres-acceptance-2026-11-14.md)。

## 7. 尚未验收

- 生产 PostgreSQL、生产反向代理/中继和生产服务端升级。
- 两台不同网络主机、NAT/公网 IPv6、弱网和真实 Flutter 查看器整链。
- 真正更换 Ed25519 私钥后的分发、重叠期、退役、回滚和备份恢复。
- 高可用、主从切换、连接池容量、TLS 证书和长时间运行。

在上述项目完成前，ACL 状态保持 **部分完成**，不能将本机回环结果外推为生产
可用性结论。
