# ACL 真主机单机回环验收（2026-11-13）

## 1. 结论与证据等级

2026-11-13 使用真实 `chast-server.exe`、真实 `chast-host.exe`、真实
`chast-tunnel.exe host` 和真实 `chast-quic connect` 完成受管准入的单机回环验收。
本文件记录账号封禁、设备禁用与恢复、账号组授权与移组撤权、运行中权限收窄、
越权拒绝，以及签发 kid 退役与恢复。

结论：ACL 主链已从“源码完成、回环自动化”推进到“真主机进程单机回环通过”。
2026-11-14 又在本机 PostgreSQL 14.25 真库上完成了同一组 ACL 整链复验，覆盖
权限收窄、策略撤权、账号封禁、设备禁用/恢复、账号组授权/移组、kid 退役/恢复。
生产服务端、真实反向代理、两台不同网络主机、真实新私钥轮换和 HA 仍未验收，
因此 ACL 继续保持 **部分完成**，不能将本文当作跨机或生产验收。

## 2. 环境边界

| 项目 | 本次环境 |
| --- | --- |
| 服务端 | 当前工作区构建的真实 `chast-server.exe`，SQLite 状态库 |
| 主机 | 当前工作区构建的真实 `chast-host.exe --config host.ini --tray` |
| 代理 | 真实 `chast-tunnel host`，HTTP 注册、保活、策略事件与 QUIC 本地目标 |
| 会话客户端 | 真实 `chast-quic connect --access-ticket`，实际 DXGI 捕获、H.264 编码与解码 |
| 网络 | 全部端点在 `127.0.0.1` / 单机网卡回环，不代表 NAT、代理或公网路径 |
| 策略 | `restricted`，设备策略版本 v6，权限上限 527，允许用户 `alice` |
| 主机回执 | `policy_version=6`、`applied=true` |

测试凭据、私钥、客户端种子、数据库、日志与构建产物只保留在本机被忽略的
`build/` 目录，不进入仓库。

## 3. 验收结果

| 场景 | 操作 | 实际结果 | 结论 |
| --- | --- | --- | --- |
| 权限收窄 | 运行中将会话权限从 527 收窄到 3 | 续租后权限为 3；收窄前文件列表成功，收窄后同一请求返回 `PermissionDenied`；视频继续出帧 804 帧，CRC/解码/序号/协议错误均为 0 | 通过 |
| 策略撤权 | 运行中将 `permission_ceiling` 从 527 推送为 3 并升版本 | 主机会话约 14 秒后结束，回执 `applied=true` | 通过 |
| 账号封禁 A | 建立会话后封禁 `alice` | 会话在 58.735 秒结束；接收/解码 937/937 帧，全部错误计数为 0 | 通过 |
| 账号封禁 B | 封禁前保留账号 token，建立会话后封禁 | 封禁后旧 token 申请新票据返回 HTTP 401；已有会话在 54.743 秒结束；接收/解码 889/889 帧，全部错误计数为 0 | 通过 |
| 设备禁用 | 建立会话后 `PATCH /v1/identities/aclhost` 设置 `disabled=true` | 新票据返回 HTTP 403；会话在 53.733 秒结束；接收/解码 938/938 帧，全部错误计数为 0 | 通过 |
| 设备恢复 | 设置 `disabled=false`，等待主机重新注册 | 用户认证 lookup 恢复 `online=true`，relay/punch/carrier 与 51 秒租约恢复，策略 v4 回执保持 `applied=true` | 通过 |
| 账号组授权 | 创建组并加入 `alice`，策略只允许该组 | `alice` 取得 v5 票据并建立真实会话 | 通过 |
| 移组撤权 | 会话中把 `alice` 移出允许组 | 成员删除返回 204；旧 token 申请新票据返回 HTTP 403；已有会话在 54.767 秒结束；接收/解码 1613/1613 帧，全部错误计数为 0 | 通过 |
| kid 退役 | 预置新 kid 后建立真实会话，再退役当前签发 kid | 新票据返回 HTTP 503；已建立会话在约 7.5 秒内结束；退役前已接收/解码 196 帧，错误计数为 0 | 通过 |
| kid 恢复 | 服务端以预置的新 kid 重启 | 主机重新注册并回执策略 v6 `applied=true`；新票据使用 v2 kid，真实会话接收/解码 874 帧持续 30 秒，全部错误计数为 0 | 通过 |

## 4. 关键场景说明

### 4.1 权限收窄与越权拒绝

真实会话在主机会话中途收窄权限。查看器通过新增的 `--file-list-probe` 先确认
收窄前文件列表可用，再在收到权限更新后立即请求同一操作，客户端侧返回
`PermissionDenied`。视频流没有因此中断，说明权限收窄关闭了后续敏感操作，但没有
破坏已经授权的媒体路径。

### 4.2 账号与设备撤权

账号封禁和设备禁用分别覆盖“主体失效”和“目标失效”两类准入来源。两个场景都在
存量会话建立后执行；旧会话在文档规定的 65 秒有界撤权窗口内结束，预先持有的账号
token 也无法在撤权后取得新票据。

设备恢复后，用户认证 lookup 已确认真实在线。管理端身份列表曾短暂显示
`online=false`，不作为恢复失败依据；最终以账号 lookup、注册续期和真实会话为准。

### 4.3 账号组授权与移组

策略把 `alice` 的授权来源从账号名单切换为账号组后，票据和真实会话均成功建立。
会话中移出组后，成员关系删除、旧 token 新票据拒绝和存量会话最迟 65 秒结束均闭环。
验收结束后恢复 v6 策略、删除临时组，并重新确认 `alice` 可以签发 v6 票据。

### 4.4 kid 退役与恢复

主机先在 `host.ini` 中通过重复 `access_key=<kid>:<public-key>` 预置新 kid，随后
重启并确认策略回执。旧 kid 签发的真实会话建立后，服务端通过
`POST /v1/access-kids` 退役旧 kid：

- 新票据立即返回 HTTP 503；
- 退役记录持久化，`access_kid_revoke` 审计落库；
- 已建立的受管会话在约 7.5 秒内被撤销推送终止，而不是等待 65 秒上限；
- 服务端以新 kid 重启后，主机重新注册，新票据和真实视频会话恢复。

本次轮换复用同一 Ed25519 公钥，只改变 `kid`，用于验证 kid 退役与主机重叠信任
路径；没有验证真正更换私钥后的分发、备份和回滚流程。

## 5. 证据边界

- `chast-quic connect` 在有限观察窗口结束时可能报告
  `stream did not end cleanly` 并返回非零退出码。该退出状态来自测试工具的观察窗口，
  不能单独判为 ACL 失败；本文件的通过依据是会话准入、实际帧数、错误计数和撤权
  时间。需要干净关闭的正式会话仍应使用 Flutter 查看器复验。
- 设备恢复后的管理端身份列表可能短暂滞后；以认证 lookup 和实际会话为准。
- 本机为单机回环和单显示器，没有跨 NAT、公网 IPv6、真实中继节点、弱网或双机
  查看器证据。
- 2026-11-13 的 ACL 整链使用 SQLite；2026-11-14 已在本机 PostgreSQL 14.25
  真库上重跑 ACL 整链，结果见本文第 6 节和
  [PostgreSQL 13/14 真库验收](postgres-acceptance-2026-11-14.md)。生产反向代理、
  生产服务端升级和跨机网络仍未验收。
- 尚未完成真实 Flutter 查看器与真实代理、中继组合的整链验收，也没有把本次结果
  外推为生产可用性结论。

## 6. PostgreSQL 14.25 本地真库 ACL 复验（2026-11-14）

### 6.1 环境与拓扑

| 项目 | 本次环境 |
| --- | --- |
| PostgreSQL | 14.25，PID 11040，`127.0.0.1:55414` |
| 数据库 | `chast_acl_20261114` |
| 服务端 | 当前工作区构建的真实 `chast-server.exe`，PostgreSQL 后端 |
| 主机 | 真实 `chast-host.exe`，PID 4456；配套隧道 PID 8 |
| 主机 QUIC | `127.0.0.1:25433` |
| 服务端端点 | HTTP `127.0.0.1:28443`、punch `28444`、carrier `28445`、admin `28446` |
| 会话客户端 | 真实 `chast-quic connect --access-ticket`，实际 DXGI 捕获、H.264 编码与解码 |
| 最终票据 | `kid=acl-e2e-v2`，`policy_version=7` |

测试库、构建产物、密钥、日志和错误输出均位于仓库外临时目录，不进入仓库。

### 6.2 验收结果

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

测试工具在有限观察窗口结束时可能返回 1 并报告
`stream did not end cleanly`；该退出状态来自观察窗口，不作为 ACL 失败依据。
通过依据是准入结果、实际帧数、错误计数和撤权时间。

### 6.3 PostgreSQL 布尔类型缺陷与修复

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

### 6.4 最终状态与证据边界

验收结束后删除临时管理员 `acladmin`，最终只有 `alice`，管理员、未封禁、
session cap 0；临时组已删除，策略恢复为 v7，服务端以 `kid=acl-e2e-v2` 运行。

本次仍是本机回环、单显示器、单 PostgreSQL 实例，不是生产 PostgreSQL、真实反向
代理/中继、双机 Flutter、真实新私钥轮换或 HA 验收。ACL 总体状态保持
**部分完成**。

## 7. 后续收口

1. 在真实反向代理和生产服务端上，使用 PostgreSQL 重跑撤权、kid 轮换与策略回执。
2. 使用两台不同网络主机和真实 Flutter 查看器覆盖直连、打洞、relay、TCP/WS
   carrier，以及会话中撤权和网络切换。
3. 验证真正更换 Ed25519 私钥时的 pre-publish、重叠期、退役、回滚与备份恢复。
4. 在生产整链通过前，ACL 状态保持“部分完成”。
