# 当前状态与后续计划（2026-11-12，工作区检查点）

本文件记录 `main` 分支在 2026-11-12 的服务端/管理台工作状态与后续计划，用于在并行工作流继续推进协议 v72 期间保存本工作流的成果与边界。协议与原生代码的实时状态仍以 `docs/status-and-plan-2026-11-09.md` 和代码为准。

## 1. 分支与提交基线

- 仓库：`ltpara/ds-chcast`；分支：`main`；最近已提交基线：`ca51be2`（`chast: draw cursor hint overlay with per-pixel transparency`），协议 **v71**。
- 本次提交范围：`server/**`（Go 服务端、管理台静态资源、`server/README.md`、`go.mod`/`go.sum`）与本文档。
- 工作区同时存在并行工作流的未提交改动（协议 v72、`src/**`、`include/chast/protocol.h`、`app/**`、`host/**`、原生测试与部分对照文档）；这些文件不属于本次提交，也未被修改或回退。
- `docs/status-and-plan-2026-11-09.md` 与 `docs/rustdesk-full-parity-2026-11-07.md` 由两条工作流共同编辑（含 CRLF 规范化），本次未纳入提交；本工作流的增量摘要见本文第 3 节。

## 2. 本次提交的服务端能力

| 能力 | 对应条目 | 关键实现 | 验证 |
| --- | --- | --- | --- |
| LDAP/AD 目录登录 | S11/G14 | `server/internal/ldapauth`（bind → 转义过滤检索 → 以用户 DN 重绑）、`server/internal/ldaptest` 进程内目录、账号 schema v17 `ldap_identities`、`--ldap-*` 参数与登录回退/503、审计 | ldapauth 单元、账号与 API `TestLoginLDAP*`、管理台断言、真实二进制 503 冒烟 |
| 设备集中禁用与属主 | S18 | registry schema v3 `disabled`/`owner`、`SetDisabled` 释放租约、注册拒绝、`deviceAdmission` 让属主放行、`PATCH /v1/identities/{id}`、审计与唤醒 409 | registry/API 测试、真实二进制 30 项冒烟（含重启持久化） |
| 设备批量禁用/启用 | S18 | `POST /v1/identities/bulk`（1–200 去重、`updated`/`missing` 部分成功、分组越界 403、逐台审计）；管理台勾选、全选与二次点击确认 | `TestBulkDeviceDisable`、管理台静态断言、数据库级冒烟 |
| 设备清单 CSV 与分页 | S18 | `GET /v1/identities?format=csv`（13 列、`csvSafe`、忽略分页）；管理台 200 台/页、上下页、越界回退 | `TestDeviceInventoryCSVExport`、205 台真实二进制分页冒烟 |
| 审计/连接/文件时间窗 | S22 | `account.TimeBounds`、`*Range` 查询变体、`reportTimeBounds`（毫秒或 RFC3339、非法窗口 400）、JSON 与 CSV 共用 | `TestReportTimeRangeFiltering`、真实二进制窗口冒烟 |
| 账号审计用户名过滤 | S22 | `AuditEventsForBeforeRange`/`AuditEventsForGroupBeforeRange` 增加精确 `user`，与动作/分组/时间/游标组合；`/v1/audit` 与 CSV 接受 `user`；管理台审计页签账号输入 | `TestAuditEventsForBeforeRangeFiltersByUser`、`TestAuditUserFilter`、真实二进制 24 项冒烟 |
| 账号目录分页/搜索/导出 | S24/G07 | `Store.ListUsersPage`（同一 WHERE COUNT + 分页、LIKE 通配符转义、角色校验）、`q`/`role`/`limit`/`offset`/`total`、`format=csv`；管理台搜索防抖、角色筛选、翻页与导出 | `TestListUsersPage`、`TestAdminAccountDirectoryPagingAndCSV`、16 项冒烟 |
| 管理台账号登录会话 | S24 | `GET /v1/accounts/{u}/sessions`、单条撤销、`revoke-all`；`Store.RevokeAllSessions`；自身会话 409、保留调用会话；对话框与审计 | `TestRevokeAllSessions`、`TestAdminAccountSessions`、21 项冒烟 |
| 账号级会话权限上限 | S14/G03 | `users.session_permission_ceiling`（schema v16）、准入时与设备策略取交集、`POST /v1/accounts/{u}/permissions`、列表回显 | `TestSessionPermissionCapIntersectsDevicePolicy`、`TestAccountSessionCapEndpoint` |
| 策略分配原子批量 | S15/G04 | `POST /v1/policy-assignments/bulk`（1–200、事务内 `ON CONFLICT DO NOTHING`、配额整批回滚、单条审计） | `TestDevicePolicyAssignmentBatch`、`TestBulkPolicyAssignments` |
| 区域优先中继租借 | S06/G13 | `--relay-node-regions` 把节点绑定到 GeoIP 标签，注册时优先同区域健康节点，粘性与回退不变，`/v1/server`、`/v1/stats` 暴露 `region` | relaypool 单测、`TestRegistrationLeasesRelayInHostRegion` |
| 服务端 NAT/信号补齐 | S03/G13 | LAN 掩码与 XOR、可信代理头、注册 ID 节流、GeoIP 热重载、`FetchLocalAddr` 新鲜度、`/chast-signal` WebSocket 会合 | 对应 API/rendezvous/agent 测试与真实二进制冒烟（详见状态文档） |

## 3. 本次提交前的验证状态（2026-11-12）

- `go build ./...`（`server/`）通过。
- `gofmt -l internal/account internal/api internal/webadmin` 干净；`go vet ./internal/account/ ./internal/api/ ./internal/webadmin/` 通过。
- `go test ./internal/account/ ./internal/api/ ./internal/webadmin/ -count=1` 通过（最近一次：35.10 s / 59.92 s / 0.059 s）。
- 管理台脚本 `node --check`（Qoder 内置 Node）通过。
- 最近一轮真实二进制冒烟：账号审计用户名过滤 24/24 通过（匿名 401、非法 `limit` 400、账号+动作+limit、未知账号空结果、CSV 仅目标账号、admin 令牌过滤、内嵌控制台资源）。
- 边界说明：本轮未重跑完整 `go test ./...`（全仓库），也未运行 Flutter/原生构建；这些属于并行工作流范围或需在其 v72 合并后统一复验。

## 4. 未提交内容与边界

- 并行工作流：协议 v72 相关原生代码、采集/QUIC/查看端引擎、`app/**`、`host/**`、原生测试及其对照文档均未提交，因此 GitHub 上 `main` 的协议版本仍是 v71。
- 文档：`docs/status-and-plan-2026-11-09.md` 与 `docs/rustdesk-full-parity-2026-11-07.md` 含两条工作流的编辑与行尾规范化，未纳入本次提交；本次增量以本文与 `server/README.md` 为准。
- 环境限制：本机只有单显示器、Intel GPU，无合成输入/锁屏/断网；临时服务器无 SMTP/SMS；自动化浏览器在早期原生 `confirm` 后卡死，管理台仍以静态断言与 HTTP 冒烟替代真实点击。
- 运行时文件（`chast-server-identity.key`、`chast-server-state.json`、`*.pid`、`mp4_recorder_*.mp4` 等）保持未跟踪、未提交。

## 5. 后续计划

1. **G19 收尾（需与并行工作流协调）**：AV1 4:4:4、远程打印、Switch Sides 三项都涉及 `include/chast/protocol.h`、`src/**`、`app/**`，属于并行工作流文件。待其 v72 提交后，选择一项按其协议节奏实施；优先评估远程打印（主机端生成 + 查看端查看，不改变零拷贝媒体路径）。
2. **S20 共享密码**：需要用户先决定密钥交换/加密取舍（服务端托管 vs 端到端），决定后实现账号级共享密码、审计与查看端入口。
3. **环境验收清单**（代码已具备，缺真实环境）：真实 SMTP/SMS 发信、AD/OpenLDAP + 证书链/StartTLS、企业 IdP（Keycloak/Entra）、硬件认证器、多设备策略回执收敛、双机会话与真实中继网络。
4. **服务端候选增量（不触协议，可与并行工作流并行）**：账号目录增加邮箱/手机过滤与排序；会话列表分页与在线状态；审计导出按主机/分组组合过滤；策略对象导入/导出；地址簿共享的组/Everyone 撤销审计细化。
5. **同步与提交纪律**：并行工作流提交 v72 后先 `git fetch` 比较 `origin/main`，确认分支包含关系再决定 fast-forward/rebase；本工作流继续只提交 Go 服务端与管理台范围，不暂存、不回退原生文件。

## 6. 常用验证命令

```bash
cd server
go build ./...
gofmt -l internal/account internal/api internal/webadmin
go vet ./internal/account/ ./internal/api/ ./internal/webadmin/
go test ./internal/account/ ./internal/api/ ./internal/webadmin/ -count=1
ELECTRON_RUN_AS_NODE=1 "D:/Programs/Qoder/Qoder.exe" --check internal/webadmin/assets/app.js
```

真实二进制冒烟：使用临时 SQLite（`--state <tmp>/state.db --server-key <key>`）、回环监听端口，按对应条目脚本验证授权、过滤、CSV 与控制台资源；冒烟后结束进程并删除临时目录。
