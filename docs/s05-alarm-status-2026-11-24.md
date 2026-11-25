# S05 安全告警首个垂直切片

日期：2026-11-24
分支：`codex/complete-workspace-2026-11-20`
状态：**代码和自动化门禁已通过；PostgreSQL、SMTP 与部署验收仍在进行**

2026-11-24 历史源码对账：当时 checkout `dc63487` 的 schema 最高为 v28，尚未包含本文描述的 S05 代码。2026-11-25 GitHub 同步已将 S05 实现纳入当前分支；以下实现及测试状态以本页后续记录和对应提交为准。

## 已交付

- schema v29 增加独立 `alarm_events` 表。告警不依赖账号审计表的读取或保留；来源审计记录被修剪后，告警仍保留在自己的 10,000 条有界事件流中。新增独立 `--alarm-retention-days` 按天清理告警，默认 0 只保留 10,000 条上限；告警清理周期与账号审计历史独立配置。
- 审计写入与告警投影处于同一数据库事务。当前分类为：
- `host_denied` → `security / warning / host_access_denied`；设备签名报告携带稳定来源键，重试只生成一条告警。
- `emergency_access` → `access / notice / emergency_access_granted`。
- 登录失败达到现有 `LoginLimiter` 阈值 → `security / warning / account_login_locked`。分类只接收阈值触发审计的 `; locked out` 后缀；锁定期间的后续请求写入独立的 `locked out` 明细，不重复生成告警。
- 成功的账号密码重置、关闭/重置双重验证、移除 WebAuthn 凭据 → `account_password_reset`、`two_factor_disabled`、`two_factor_reset`、`webauthn_credential_removed`。
- 成功的 Admin Role 变更/绑定、主机策略或设备启停 → `admin_access_changed`、`host_access_changed`；成功撤销访问票据签名密钥 → `access_signing_key_revoked`。
- 以上管理变更告警只由成功审计产生，普通失败校验仍保留在审计中，不逐次转成通知。
- `GET /v1/alarms` 支持 ID 游标分页、类别、级别、事件代码、用户名、主机和时间范围过滤；`format=csv` 导出相同过滤结果。
- 查询沿用 `audit.read` 权限、现有审计分组范围和审计 API Token 限定；没有告警专属权限绕过。
- 管理台“审计”页新增“安全告警”子页、分类/级别选择器、主机/用户名/时间过滤、分页和 CSV 导出。
- SQLite 与 PostgreSQL 都有 schema v29 告警迁移。schema v31 再增加持久邮件投递任务和账号订阅字段；PostgreSQL 集成测试覆盖告警及投递任务，但当前未设置 `CHAST_TEST_POSTGRES_DSN`，尚未对实库运行。
- 可选 `AlarmQueue` enqueue 接口仅在事务提交成功且实际创建新告警后回调；重复来源不会重复回调，回调错误只记日志、不改变原 HTTP 结果。邮件持久任务独立于此接口，在告警事务内为符合条件的订阅者写入 `alarm_email_deliveries`；后台 SMTP worker 使用数据库租约领取任务，记录成功/失败状态并按指数退避重试。投递目标在入队时快照；发送前会重新核对订阅、已验证邮箱和全局 `audit.read` 权限，邮箱变更或关闭订阅会取消尚未发送的任务。
- 告警邮件订阅 API 已接通 Flutter 找回邮箱页和管理台“我的账号”。仅拥有全局审计读取权限、邮箱已验证且 SMTP 可用的账号能开启订阅。

## 本轮验证

- `go test ./internal/account ./internal/api -count=1`：通过。
- `go vet ./internal/account ./internal/api ./internal/testutil`：通过。
- `go test ./internal/account -run '^TestAlarmAgeRetentionIsIndependentFromAuditRetention$' -count=1`：通过。
- `go test ./internal/admin -run '^TestAuditConsolePrune$' -count=1` 和 `go test ./cmd/chast-server -count=1`：通过；服务保留参数接线及管理台原审计清理语义回归通过。
- SQLite v28→v29 迁移、告警投影事务回滚和来源审计被清理后的告警保留回归：通过。
- `TestAlarmAgeRetentionIsIndependentFromAuditRetention`：验证审计按期清理不会删除告警；独立告警保留天数只删除过期告警。
- `TestAlarmClassifiesSuccessfulSecurityAndAccessChanges`：账号凭据、管理员权限、主机策略与签名密钥分类通过；失败的安全设置操作不生成告警。
- 现有限流器触发账号锁定时生成一条告警、锁定期间后续尝试不重复告警：`TestAccountLoginLockout` 通过。
- PostgreSQL `TestPostgresAlarmEventsFromAudit`：已加入 account 测试；当前未设置 `CHAST_TEST_POSTGRES_DSN`，实库测试没有执行。
- `node --check internal/webadmin/assets/app.js`：通过。
- `node --test internal/webadmin/app_session_test.cjs`：当前 51/51 通过，覆盖告警筛选、邮箱验证和告警邮件订阅 UI；早期 S05/S06 结果保留各自日志范围。
- S05/S06 基线切片的 Flutter 全量 `flutter test --no-pub`：877/877 通过；对应 `flutter analyze --no-pub` 通过。恢复邮箱页与 Server API 定向测试 131/131 通过。这些结果早于 v33 登录设备白名单接线。
- 告警邮件订阅、持久任务、SMTP worker 成功落库、租约、重试和发送前资格检查的 Go 回归均通过；Go 全量 `go test ./... -count=1 -timeout 10m` 与定向 `go vet` 已通过。
- `git diff --check`：通过。
- 告警通知队列：账号锁定阈值事件入队一次，队列返回错误时登录仍返回原有 429；主机拒绝重试仅入队新建事件，且队列回调能查询到已提交的告警。
- 早先服务端 `go test ./... -count=1` 曾因 `TestTCPPunchThroughRendezvousWhenUDPExpired/tcp` 进入不符合预期的直接 TCP 路径而失败，单项重跑通过。2026-11-24 在 S06 接线后的完整工作区再次执行 `go test ./... -count=1 -timeout 10m`，全部包通过；完整日志为 `build/s06-email-go-full.log`。

## 剩余工作

1. 继续对照剩余安全审计动作与服务端运行事件，按运维价值补充告警分类；每类需有可解释代码、级别和脱敏摘要。
2. 登录锁定告警继承现有限流阈值和锁定周期；主机报告按来源键去重。跨事件聚合/风暴控制仍未定义，需明确告警频率和恢复行为，来源幂等不能替代风暴控制。
3. 为不同环境选择 `--alarm-retention-days`；默认 0 保留最近 10,000 条，正值按配置天数定期清理。生产保留时长仍需结合部署要求确定。
4. 在 PostgreSQL 实例上执行告警和投递任务迁移、重复迁移、事件写入/回滚、权限筛选分页、投递领取/重试测试，并验证与 SQLite 行为一致。
5. 在真实 SMTP 环境验收投递与失败恢复，并明确 SMTP 成功但状态回写失败后的重复发送语义；当前任务可租约恢复并重试，但邮件协议本身无法提供 exactly-once 保证。
6. 补部署浏览器验收，包括撤权后刷新、分组账号可见范围、空结果与 CSV 内容，以及邮件偏好开关后的状态。

首批安全事件和本轮扩充均基于现有明确审计动作；管理员变更只报告成功操作。普通登录失败和失败的管理操作仍留在审计，不逐条转成告警或通知。其他服务运行事件仍需逐类确认运维价值、脱敏摘要和触发频率后再加入目录。

## 2026-11-24 后续工作区门禁补充

随后接入的 v33 登录设备白名单尚未提交。该工作区的 Go 全量测试和 Windows Release 构建通过；Flutter 最新全量运行有两项安全密钥登录测试失败，`flutter analyze --no-pub` 报告六项 lint 提示。因此上方 877/877 是 S05/S06 切片基线，不代表当前整个工作区门禁通过。PostgreSQL 实库、真实 SMTP 和部署浏览器验收仍待完成。账号安全功能的优先级和后续顺序见[账号安全与企业治理计划](account-security-priority-and-next-plan-2026-11-24.md)。

## 2026-11-25 门禁与恢复界面更新

上节记录的问题已修复：既有安全密钥登录测试使用无设备证明测试 provider，设备证明 API 由单独测试验证；Flutter 全量 880/880、`flutter analyze --no-pub` 无问题。管理台现已补齐管理员登录设备列表、公钥登记、白名单启停和设备撤销；权限区分全局 `accounts.read` 与 `accounts.write`，启停/撤销会话影响有确认提示，无设备时不能启用。Go 全量及 `go vet ./...`、管理台 Node 53/53 和 `cmake --build build --config Release --parallel 4` 均通过。PostgreSQL 实库、真实 SMTP、部署浏览器和双机恢复仍待完成，详见[当前完成状态与后续计划](current-status-and-next-plan-2026-11-17.md)。
