# 新分支修改与既定执行方向审核

日期：2026-11-17

审核版本：`codex/s03-role-console-2026-11-17`，HEAD `202a2284962d8602b6d9378cdd78e7ddd6265b2b`。

比较基线：`c3490f86050d09876692067344b7dc3e3ce637ad`（上次提交的 F1–F4、S01、部分 S02 和执行方案）。范围为基线之后 6 个提交、55 个文件，重点审核阶段方向、S03 权限及其与 S02 的衔接。

## 结论

产品范围和实施主线没有明显偏离。新增工作集中于 W1/M1 的 S02 活动连接控制、S03 命名管理员角色，以及支撑它们的测试隔离和诊断。未看到扩展跨平台、官方 RustDesk 协议兼容、密码同步或其他条件项目。Flutter 查看端在这 6 个提交中没有改动。

但 S03 的权限隔离、作用域可用性和机器令牌兼容存在已复现的代码缺陷，未达到原方案“所有 API 服务端鉴权、保持既有角色语义、越权和跨组回归通过”的门槛。当前“代码闭环完成，只剩外部验收”的表述过早。建议将 S03 调整为“主体已实现，权限缺陷修复中”；S02 增加“S03 接入后的机器令牌断开回归待修复”。在关闭这些问题前继续留在 W1。

审核依据以基线版本中的 [执行方案](rustdesk-execution-plan-2026-11-16.md) 为准，再对照当前 [状态总览](feature-status-and-plan-2026-11-16.md) 和 [S03 状态记录](s03-role-console-status-2026-11-17.md)，避免用改写后的计划证明改动本身符合原计划。

## 方向对应关系

| 实际改动 | 原定工作 | 评价 |
| --- | --- | --- |
| S02 指令重投递、确认字段、Go 代理、原生 runtime/连接 ID、精确断开、管理台 | S02-A–F | 属于原定代码补齐；源码路径存在，仍需实机和故障恢复验收 |
| CTest 临时目录隔离、QUIC 端口隔离、音频过期诊断和回归 | S02-G、V 系列证据 | 是支撑门禁的修改，没有扩大产品范围或修改既定性能阈值 |
| 稳定权限键、命名角色、绑定、管理台角色页面 | S03 | 方向正确，但下列权限和消费端缺陷阻止阶段收口 |
| PostgreSQL、双机、重启、断网、浏览器 E2E 继续标待验收 | 原定证据分层 | 符合方案；不能以 API 和静态页面测试替代 |
| S02 代码补齐后继续做 S03，外部验收并行保留 | 原执行方案 §3.1、§4.2 | 原方案允许实机证据和后续代码开发并行，不构成单独的方向偏离 |

## 已复现的问题

### R1 / P1：账号写权限可绕过角色权限上限，自行升级为 admin

位置：`server/internal/api/accounts.go:285`，`server/internal/api/accounts.go:295`；创建账号路径同样需要检查。

给普通账号绑定仅含 `accounts.write` 的全局命名角色后，访问角色列表原为 HTTP 403；该账号调用 `PATCH /v1/accounts/limited`，提交 `{"role":"admin"}` 得到 HTTP 200；使用原有 token 再访问角色列表得到 HTTP 200。

角色 API 的权限上限检查没有覆盖旧的固定角色修改入口。被限制在账号管理范围的主体可以获得全部设备、录制、审计及角色管理权限。`POST /v1/accounts` 也直接接受角色参数；密码重置等高权限账号管理行为需要一并明确目标权限边界。

修复方向：区分普通账号维护和管理权限授予，对固定角色变更、带角色创建、特权账号凭据管理实施统一的权限上限与目标保护。不能只在命名角色 API 检查提权。

### R2 / P1：断开连接权限被用来签发应急访问票据

位置：`server/internal/api/emergency_access.go:48`。

仅有 `connections.disconnect` 的账号对不在其设备访问白名单内的受管设备请求 `/v1/emergency-access`，实际得到 HTTP 201 和访问票据。应急入口明确绕过账号 allow list；本地认证和设备权限上限仍会执行，但这不改变“断开权限额外获得应急准入”的事实。原先固定 operator 没有此管理员能力，现在 operator 的权限集合也包含该键。

修复方向：为应急访问定义独立、明确的授权，保持原管理员边界，并按目标设备校验作用域。不能把停止会话和突破常规准入策略合并为同一权限。

### R3 / P1：设备删除权限仅在前端体现，后端仍接受 devices.write

位置：`server/internal/api/api.go:1081`、`:1112`；前端为 `server/internal/webadmin/assets/app.js:1455` 附近。

实际请求 `DELETE /v1/identities/devbox`：仅有 `devices.write` 返回 HTTP 204，删除身份成功；仅有 `devices.delete` 返回 HTTP 403。管理台却使用 `devices.delete` 决定“重置身份”和“移除租约”是否可用。

这既允许编辑角色越权执行删除，又让合法删除角色无法操作，违反原方案“不能只隐藏按钮”的要求。

修复方向：设备身份删除和租约移除的后端检查使用对应稳定权限，并补前后端权限矩阵及越权回归。

### R4 / P2：分组/对象授权没有贯通管理台登录和资源列表

位置：`server/internal/api/account.go:475`、`server/internal/api/live_connections.go:42`、`server/internal/webadmin/assets/app.js:504`；组列表筛选位于 `server/internal/api/groups.go`。

给普通账号绑定仅在 east 组生效的 `connections.read`、`connections.disconnect`：

- 同组连接详情返回 HTTP 200，证明作用域授权已生效；
- 活动连接列表返回 HTTP 403；
- 登录返回的全局 `permissions` 为空；
- `/v1/groups` 返回 HTTP 200，但组列表为空。

管理台的实际登录代码会在“全局权限为空且没有可见组”时拒绝该账号。只有对象授权的账号也缺少相应登录发现路径。这不是缺一个浏览器验收记录，而是身份、资源发现和 UI 权限模型未完整衔接。

修复方向：返回可用的作用域能力，支持经过授权过滤的列表和导航；不能通过额外授予全局权限来绕过。需要覆盖仅组角色、仅对象角色、跨组拒绝和撤权。

### R5 / P2：机器令牌的资源过滤移除了 connections.disconnect

位置：`server/internal/api/console.go:201`、`:211`；`server/internal/account/admin_roles.go:137`。

固定 admin 创建的 `resources:["all"]`、`role:"admin"` 机器令牌调用有效目标的定向断开接口，实际返回 HTTP 403。接口传入 devices 资源族，资源过滤只保留 `devices.*`，随后只补回连接读取权限，没有补回 `connections.disconnect`。旧实现按 consoleWrite 校验，此处构成 S03 对既有 S02 能力的兼容回归。

修复方向：明确连接操作归属的机器令牌资源与动作权限，同时保持 owner 权限上限、token 角色上限和组范围约束。

## 文档和验收口径

1. S03 状态记录仍写 PostgreSQL **v23** 迁移，但 `server/internal/account/migrate.go` 的角色迁移是 **v24**。应覆盖新建、23→24 升级、重复迁移、备份恢复及降级边界。
2. 执行方案和状态总览引用的 `9ec70b0` 在当前已获取的 Git 对象中无法解析；当前历史中测试隔离提交是 `2a5ffe7`。文档还保留旧分支名和“领先远端 5 个提交”，不符合本次审核的分支状态。
3. 原生全量 79/82 等数据属于文档中的历史记录，本次未重跑，且本地未找到所引用的 `build/ctest-full-20261117.log`。应补齐可追溯的提交、命令和原始日志；不能据此推断当前 HEAD 的所有门禁均已通过。
4. 新发现的越权问题按原方案应自动优先于功能扩展。建议先关闭 R1–R3，再处理 R4–R5、权限矩阵与文档，之后继续 PostgreSQL、浏览器和双机验收。C01/W2 的产品方向无需调整，但不宜现在宣称 W1 已收口。

## 本次验证与边界

实际通过：

```text
node --test internal/webadmin/app_session_test.cjs
  9 passed, 0 failed
node --check internal/webadmin/assets/app.js
go test ./internal/account ./internal/api ./internal/agent ./internal/webadmin \
  -run 'Test(Named|UpdateAdminRole|GroupScopedManager|ObjectScoped|AdminRole|Role|Live|Emergency|APIToken|Console)' \
  -count=1 -timeout 5m
  四个包的匹配用例通过
git diff --check
```

为审核新增了 5 个临时 Go 定向测试函数（设备删除含两个子场景），使用隔离测试服务和数据库复现 R1–R5。执行 `go test ./internal/api -run '^TestDirectionReview' -count=1 -v -timeout 3m` 时，五类预期均失败；上述 HTTP 状态来自真实本地请求，不是静态推测。

临时测试已从源码目录移除，副本保留在 Git 忽略的 `build/review-direction-20261117/permission_probes_test.go`，可在修复时转为正式回归。本次没有修改业务代码、调整原方案状态或提交推送；新增本审核记录供后续执行参考。

本次没有执行 PostgreSQL、真实 Windows 双机、浏览器 E2E、原生全量构建或 RustDesk 性能 A/B。S02 原生链路进行了源码检查，不代表其故障恢复和实机行为已全部通过。


## 修复结果（2026-11-17）

以下记录为上述审核后的修复，不改写原始复现结果。分支保持 `codex/s03-role-console-2026-11-17`，修复基线为 `202a228`；本轮尚未提交或推送。

| 编号 | 修复结果 | 主要回归 |
| --- | --- | --- |
| R1 | 账号维护与固定角色授予增加 `roles.write`/目标权限上限；覆盖恢复邮箱、手机号、密码、2FA、封禁、删除及会话操作；机器令牌不能借账号、角色或派生令牌逃逸自身权限 | 自提权、创建 admin、维护固定/作用域特权账号均拒绝；普通账号创建和维护可用；完整管理员可授权；最后管理员保护覆盖部分权限与多绑定并集 |
| R2 | 新增 `devices.emergency_access`，按实际目标设备判断授权；v25 刷新已有内置角色 | 仅断开权限拒绝票据；组内显式授权成功、跨组拒绝；机器令牌组范围及撤权 |
| R3 | 身份删除和租约移除使用 `devices.delete` | write-only 拒绝，两种 delete-only 操作成功 |
| R4 | 所有登录方式、刷新和账号信息返回独立作用域授权；设备/连接集合先过滤再分页；前端支持作用域导航和逐行操作权限 | group/object 登录、列表/总数/分页/CSV、策略列表、跨组及批量拒绝、解绑后拒绝；权限轮询清除旧行和选择，换号不接受旧响应 |
| R5 | devices 资源族明确包含连接读/断开；保持 owner、角色、资源和分组交集 | admin/operator 可断开；readonly、错误资源、跨组和所有者降权后拒绝；撤销令牌失效 |

新增正式回归位于 `server/internal/api/permission_regression_test.go`（9 个测试函数，含 group/object、删除和机器令牌矩阵），并扩展账号层迁移、最后管理员以及管理台会话测试。密码、OIDC 和安全密钥登录共用权限响应构造，防止不同登录方式遗漏授权信息。

迁移与产品边界：当前 schema v25；SQLite 24→25 升级及重复打开保留自定义角色/绑定。自定义角色不会自动获得应急权限；如需该能力，由持有相应权限的管理员或服务器密钥显式授予。前台可见页面每 15 秒轮询权限，服务端每次请求重新鉴权。

原生/Flutter 没有改动，本轮不以历史构建、CTest 或 Flutter 结果代替新验证。PostgreSQL 实库、浏览器 E2E、双 Windows、重启/断网、生产发布和多实例一致性仍需外部验收；W1 保持未完成发布验收，继续按原方向推进，不提前进入 C01/P2。


本轮最终验证（在修复后的工作区实际执行）：

```text
# 在 server/ 目录执行
go test ./... -count=1 -timeout 10m     # 通过
go vet ./...                           # 通过

# 在仓库根目录执行
node --check server/internal/webadmin/assets/app.js
node --test server/internal/webadmin/app_session_test.cjs
# 14 tests, 14 pass, 0 fail
git diff --check                       # 通过
```

首轮全量测试发现邮件恢复服务未配置时的 503/404 响应顺序回归，修复完整权限凭据的目标预检查后再次执行全量测试通过。PostgreSQL 集成测试因未配置 `CHAST_TEST_POSTGRES_DSN` 跳过，不计作 PostgreSQL 验收通过。Node 测试执行真实前端函数并隔离 DOM 副作用，不替代浏览器 E2E。
