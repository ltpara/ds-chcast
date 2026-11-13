# 当前状态与后续计划（2026-11-12，服务端检查点；2026-11-14 更新）

本文件记录服务端/管理台在 2026-11-12 的检查点，以及 2026-11-13/14 的续办结果。历史提交基线保留为协议 v72；当前 GitHub 基线为 `5590d75`（协议 v73），工作区源码为 v76。

2026-11-14 G21 原生增量：工作区协议 v76 新增临时自由手写标注，含查看端绘制队列、Windows 逐显示器点击穿透覆盖层、按 stroke ID 支持多窗口交错笔画和 2048 笔/65536 点上限。协议/光栅/历史测试、12/12 Flutter 定向测试、Windows Release 构建、单机真实 QUIC 回环 104 项及启用覆盖层探针后的 105 项通过，其中真实 `HostServer` 单机可见覆盖层完成窗口、像素与销毁自动化；真实双机、多屏/混合 DPI、锁屏/UAC 和生产发布仍是未完成验收，见 [临时自由标注](annotation-2026-11-14.md)。协议与原生代码的实时状态仍以 `docs/status-and-plan-2026-11-09.md` 和代码为准。

2026-11-14 P2 指针续验：Windows 主机现在同时读取 `CursorBaseSize` 与辅助功能 `CursorSize`，按有效 DPI 派生目标物理尺寸；GDI 与 DXGI 共用等比缩放、预乘 BGRA、反色掩码、热点映射和描边逻辑，发布精灵上限 512 px。真实 GDI 在辅助功能 1/13/15 档分别采集到 32/224/256 px 的有效可见形状，内容 ID 各不相同，测试后已恢复用户设置。`cursor`、`protocol`、`protocol_fuzz`、`capture_transform`、`shared_capture`、`shared_capturer`、`desktop`、`desktop_stream` 共 8/8 CTest 通过。当前会话中的 DXGI 连续 30 次没有 pointer shape，且没有致命采集错误，因此真实 DXGI 大光标、查看端叠层缩放和 hotspot 对齐仍待交互式桌面验收，见 [指针四项复核](pointer-acceptance-2026-11-11.md)。

2026-11-13 服务端增量：账号目录的 `q` 已扩展到用户名/邮箱/手机，新增独立 `email`/`phone` 过滤、五种排序键与升降序，空联系方式始终排在最后，用户名/id 提供稳定分页决胜；管理台加入联系方式搜索与排序控件，CSV 跟随同一过滤和排序。验证：`go test ./internal/account/ ./internal/api/ ./internal/webadmin/ -count=1`、`go build ./...`、`go vet ./internal/account/ ./internal/api/ ./internal/webadmin/` 和 `node --check internal/webadmin/assets/app.js` 通过。

2026-11-14 服务端增量：账号 schema 升到 v18，`user_tokens` 增加 `login_at` 与 `last_seen_at`，旧行由迁移回填；账号自助和管理员会话列表支持 `limit`/`offset`/`total`，按登录时间稳定排序，并返回最近活动与在线状态。在线表示最近 5 分钟有已认证请求，刷新令牌有效期仍决定会话是否存活。管理台两个会话视图均改为每页 20 条并补上最近活动、在线/离线与上下页。验证：账号、API、管理台三包完整测试，`go build ./...`、`go vet`、JS 语法与差异检查通过。

2026-11-14 审计过滤增量：账号安全审计、连接和文件传输的 JSON、分页与 CSV 导出现可同时组合 `group`、`user`/`username`、`host`、动作与时间窗；主机名大小写不敏感，连接/文件的主机条件继续匹配 `host_id` 或 `peer_id`。管理台“账号审计”页签开放主机输入并显示主机列。此前连接/文件视图在主机非空时会忽略用户名，本轮改为所有非空条件取交集。验证：`TestAuditEventsForFilterBeforeCombinesGroupUserHostAndRange`、`TestOperatorEventFiltersCombineUsernameAndHost`、`TestAuditReportsCombineGroupUserHostAndCSV` 与 `TestConsoleExposesAuditUserFilter` 通过，JS 语法检查通过。

2026-11-14 策略对象 JSON 导入/导出：新增 `GET /v1/policies/export` 与 `POST /v1/policies/import`，均要求全局 admin `devices` 权限。文档格式固定为 `format=chast-policy-objects`、`version=1`；导入支持默认 `conflict=reject`（同名即整包失败）和 `conflict=update`（创建/更新/未变分类）。所有校验在写事务前完成，写入在同一事务内；内容相同对象不升版本、不改更新时间。请求体限 1 MiB，拒绝未知字段与尾随 JSON，单包最多 256 个对象，只处理策略对象而不导入分配。管理台策略对象区增加 JSON 导出、文件导入及冲突模式选择。验证：策略存储与 HTTP 定向测试覆盖创建、更新、未变、整包回滚、权限、严格 JSON、1 MiB、配额、往返导入、reconcile 与审计；服务端全量测试、构建、vet 和管理台 JS 语法检查通过。

2026-11-14 地址簿组/Everyone 撤销审计：显式撤销目标授权和两条级联删除路径现在逐条记录实际删除的授权，新增 `address_book_group_revoke`、`address_book_everyone_revoke` 动作。删除前在同一数据库事务内读取属主、目标、权限和地址簿范围，再执行删除，避免查询与撤权之间的竞态；每条真实删除各写一条审计，不把批量删除折叠成父事件。`detail` 固定包含 `target_kind`、目标 ID/名称、`permission`、整本 `scope="whole"` 或 `scope_id`/`scope_name`、`owner` 与 `cause`；原因区分 `explicit_scope`、`explicit_all`、`address_book_group_deleted` 和 `account_group_deleted`。审计仍为 best-effort，写入失败不回滚已提交的撤权。验证：`TestAddressBookTargetGrantsAPI`、`TestAddressBookTargetScopeRevocationIsAudited`、`TestAddressBookTargetRevocationCascadesAreAudited` 覆盖显式整本/分组、重复撤销无额外审计、地址簿分组级联、账号组跨属主级联及原因/范围字段。

2026-11-14 地址簿原子批量移动：新增 `POST /v1/addressbook/peers/bulk-move`，一次移动 1–200 个条目到指定分组或未分组；HTTP 层去重 peer ID，每项可带条目级 `if_revision`。存储层在同一事务内校验目标分组、全部存活条目和修订号，任一未知或过期项整批回滚；实际移动项共享一个新修订号，已在目标分组的项回列 `unchanged`，全部未变化时不推进书修订。查看器“我的设备”标题栏新增“批量移动”，支持多选、全选/清空、目标分组选择、200 台上限和逐项修订提交；成功结果一次写回本地设备，冲突时刷新整批所选条目。验证：`TestAddressBookBulkGroupMove`、`TestAddressBookBulkGroupMoveAPI`（含超大请求与重复 ID）、`address book bulk move dedupes peers and parses the batch`、`bulk move sends revisions and settles all moved devices`、`settling one batch updates every moved row and shared revision` 通过。

2026-11-14 地址簿分组 JSON 导入/导出：新增 `GET /v1/addressbook/groups/export` 与 `POST /v1/addressbook/groups/import`，交换格式固定为 `format=chast-addressbook-groups`、`version=1`，每项仅含 `id`、`name`、`parent_id`、`position`。导出只含当前账号存活分组，不含设备、成员关系、名称/备注/标签、连接参数、修订、时间戳或墓碑。导入支持默认 `reject`、`skip` 和 `update`；请求体限 1 MiB、单包最多 256 项，严格拒绝未知字段与尾随 JSON，并在首次写库前完整校验 ID、名称、重复 ID、父级、自引用、环和深度 8。任一冲突或校验错误不会留下部分分组；实际创建/更新共享一个新修订号和时间戳，内容相同项回列 `unchanged`，成功结果按 `created`/`updated`/`skipped`/`unchanged` 分类并写 `address_book_group_import` 审计。查看器分组对话框新增“分组导入/导出”，提供导出预览、复制 JSON、粘贴文本、三档冲突模式和成功后重新同步，不增加文件选择依赖。验证：存储与 HTTP 定向测试覆盖创建、更新、跳过、未变、严格 JSON、1 MiB、数量上限、账号隔离、重复 ID、父级/环/深度及拒绝模式整包回滚；Dart API 与分组对话框覆盖导出、复制、三种冲突、错误提示和刷新。

2026-11-14 弱网视频帧龄回归：复现 `chast-quic verify --netem --audio` 最大帧龄 4175 ms（预算 3000 ms）。根因是视频应用层回执超过 2 s 后仍等待第二个停顿采样，且首次只按普通未确认档 0.8 降码率，导致 1 Mbps 瓶颈下积压继续扩大。`VideoQos` 现在将首个超过 2 s 的确认停顿视为紧急拥塞证据，在既有质量/1 Mbps 下限内按 0.3 强降，并用 1 s 防抖避免重复生效；2 s 内抖动与清空停顿后的正常恢复保持不变。验证：`video_qos` 新增 2 s 边界、首次强降和恢复测试并通过；`quic_weak_network`、`audio`/`audio_dsp`/`audio_playout` 通过；三种本机弱网形态最大帧龄分别为纯视频 2873 ms、DATAGRAM 音频 1956 ms、可靠流音频 1886 ms。该证据只覆盖本机单进程回环，真实双机/跨网验收仍待执行。

## 1. 分支与提交基线

- 仓库：`ltpara/ds-chcast`；分支：`main`；当前本地与 `origin/main` 基线均为 `5590d75`（`chast: close receiver status and capture gaps`），协议 **v73**。
- 服务端检查点历史提交为 `a3e17a7`（`server/**`、管理台静态资源与 `server/README.md`）和 `c625c14`（协议 v70–v72 原生代码、查看器、主机、原生测试与相关文档），不再作为当前远端基线。
- 本分支包含 v74 目录上传、v75 PNG 剪贴板、v76 临时自由标注、ACL 验收收口、账号 schema v18 会话分页、服务端导入/审计/录制续传等修改；运行测试还留下若干 `.exe`、`.obj` 和临时文件，均不得在同步或验证时清理。

## 2. 服务端检查点能力（含未提交续办）

| 能力 | 对应条目 | 关键实现 | 验证 |
| --- | --- | --- | --- |
| LDAP/AD 目录登录 | S11/G14 | `server/internal/ldapauth`（bind → 转义过滤检索 → 以用户 DN 重绑）、`server/internal/ldaptest` 进程内目录、账号 schema v17 `ldap_identities`、`--ldap-*` 参数与登录回退/503、审计 | ldapauth 单元、账号与 API `TestLoginLDAP*`、管理台断言、真实二进制 503 冒烟 |
| 设备集中禁用与属主 | S18 | registry schema v3 `disabled`/`owner`、`SetDisabled` 释放租约、注册拒绝、`deviceAdmission` 让属主放行、`PATCH /v1/identities/{id}`、审计与唤醒 409 | registry/API 测试、真实二进制 30 项冒烟（含重启持久化） |
| 设备批量禁用/启用 | S18 | `POST /v1/identities/bulk`（1–200 去重、`updated`/`missing` 部分成功、分组越界 403、逐台审计）；管理台勾选、全选与二次点击确认 | `TestBulkDeviceDisable`、管理台静态断言、数据库级冒烟 |
| 设备清单 CSV 与分页 | S18 | `GET /v1/identities?format=csv`（13 列、`csvSafe`、忽略分页）；管理台 200 台/页、上下页、越界回退 | `TestDeviceInventoryCSVExport`、205 台真实二进制分页冒烟 |
| 审计/连接/文件时间窗 | S22 | `account.TimeBounds`、`*Range` 查询变体、`reportTimeBounds`（毫秒或 RFC3339、非法窗口 400）、JSON 与 CSV 共用 | `TestReportTimeRangeFiltering`、真实二进制窗口冒烟 |
| 账号审计用户名过滤 | S22 | `AuditEventsForBeforeRange`/`AuditEventsForGroupBeforeRange` 增加精确 `user`，与动作/分组/时间/游标组合；`/v1/audit` 与 CSV 接受 `user`；管理台审计页签账号输入 | `TestAuditEventsForBeforeRangeFiltersByUser`、`TestAuditUserFilter`、真实二进制 24 项冒烟 |
| 策略对象 JSON 导入/导出 | S15/G04 | `GET /v1/policies/export`、`POST /v1/policies/import`；稳定交换格式、严格 JSON、1 MiB/256 对象上限、`reject`/`update` 冲突策略、单事务写入、未变不升版本、导入审计与管理台入口 | `TestImportPolicyObjects*`、`TestPolicyObjectImportExportRoutes`、管理台静态断言；服务端全量测试/构建/vet 通过 |
| 地址簿组/Everyone 撤销审计 | S21/G16 | 显式整本/分组撤销和地址簿分组/账号组级联删除按实际删除授权逐条写 `address_book_group_revoke`/`address_book_everyone_revoke`；事务内捕获属主、目标、权限和范围，`detail` 区分四种原因 | `TestAddressBookTargetGrantsAPI`、`TestAddressBookTargetScopeRevocationIsAudited`、`TestAddressBookTargetRevocationCascadesAreAudited` |
| 地址簿原子批量移动 | S21/G16 | `POST /v1/addressbook/peers/bulk-move` 以 1–200 条为一批，HTTP 去重并按条目校验 `if_revision`；目标分组、全部存活条目和修订在同一事务内验证，失败整批回滚，实际移动项共享新修订号，全未变化不推进书修订；查看器支持多选、目标分组与冲突整批刷新 | `TestAddressBookBulkGroupMove`、`TestAddressBookBulkGroupMoveAPI`、首页批量移动/API 解析/本地批量落盘定向测试 |
| 地址簿分组 JSON 导入/导出 | S21/G16 | `GET /v1/addressbook/groups/export`、`POST /v1/addressbook/groups/import`；仅迁移分组结构，稳定字段与顺序，严格 JSON、1 MiB/256 项上限、`reject`/`skip`/`update` 冲突策略、首次写库前全树校验、单事务共享修订号、未变不重写与分类结果；查看器复制/粘贴文本入口 | `TestAddressBookGroupImport*`、`TestAddressBookGroupExportImportAPI`、`TestAddressBookGroupImportRejectIsAtomic`、Dart API 严格解析/三冲突与分组对话框定向测试 |
| 账号目录分页/搜索/导出 | S24/G07 | `Store.ListUsersPage`（同一 WHERE COUNT + 分页、LIKE 通配符转义、角色校验）、`q`/`role`/`limit`/`offset`/`total`、`format=csv`；管理台搜索防抖、角色筛选、翻页与导出 | `TestListUsersPage`、`TestAdminAccountDirectoryPagingAndCSV`、16 项冒烟 |
| 管理台账号登录会话与分页 | S24 | `GET /v1/accounts/{u}/sessions`、单条撤销、`revoke-all`；`Store.RevokeAllSessions`；自身会话 409、保留调用会话；对话框与审计。2026-11-14 增加 schema v18 登录/最近活动时间、`limit`/`offset`/`total`、稳定登录顺序和 5 分钟在线判定，账号自助与管理员视图均每页 20 条 | `TestRevokeAllSessions`、`TestAdminAccountSessions`、早期列表 21 项冒烟；`TestSessionsPageTracksLoginOrderAndOnlineActivity`、`TestAccountSessionPaginationAndOnline`、`TestSchemaMigrationBackfillsSessionActivity` |
| 账号级会话权限上限 | S14/G03 | `users.session_permission_ceiling`（schema v16）、准入时与设备策略取交集、`POST /v1/accounts/{u}/permissions`、列表回显 | `TestSessionPermissionCapIntersectsDevicePolicy`、`TestAccountSessionCapEndpoint` |
| 策略分配原子批量 | S15/G04 | `POST /v1/policy-assignments/bulk`（1–200、事务内 `ON CONFLICT DO NOTHING`、配额整批回滚、单条审计） | `TestDevicePolicyAssignmentBatch`、`TestBulkPolicyAssignments` |
| 区域优先中继租借 | S06/G13 | `--relay-node-regions` 把节点绑定到 GeoIP 标签，注册时优先同区域健康节点，粘性与回退不变，`/v1/server`、`/v1/stats` 暴露 `region` | relaypool 单测、`TestRegistrationLeasesRelayInHostRegion` |
| 服务端 NAT/信号补齐 | S03/G13 | LAN 掩码与 XOR、可信代理头、注册 ID 节流、GeoIP 热重载、`FetchLocalAddr` 新鲜度、`/chast-signal` WebSocket 会合 | 对应 API/rendezvous/agent 测试与真实二进制冒烟（详见状态文档） |

## 3. 本次提交前的验证状态（2026-11-12）

- `go build ./...`（`server/`）通过。
- `gofmt -l internal/account internal/api internal/webadmin` 干净；`go vet ./internal/account/ ./internal/api/ ./internal/webadmin/` 通过。
- `go test ./internal/account/ ./internal/api/ ./internal/webadmin/ -count=1` 通过（最近一次：35.10 s / 59.92 s / 0.059 s）。
- 管理台脚本 `node --check`（Qoder 内置 Node）通过。
- 原生：`build/Release` 下全部二进制的时间戳均晚于当前工作树的原生源码（最后构建 2026-11-12 00:23），`chast-protocol-test` 复跑通过；`flutter analyze` 无问题（6.9 s）。
- 最近一轮真实二进制冒烟：账号审计用户名过滤 24/24 通过（匿名 401、非法 `limit` 400、账号+动作+limit、未知账号空结果、CSV 仅目标账号、admin 令牌过滤、内嵌控制台资源）。
- 边界说明：本轮未重跑完整 `go test ./...`、完整原生 `ctest` 与 Flutter 全量测试；切换机器后建议先执行本文第 6 节与完整套件复验。
- 2026-11-14 会话分页、策略导入/导出、地址簿撤销审计与原子批量移动续办：Go 服务端全量 `go test ./... -count=1`、`go build ./...`、`go vet ./internal/account/ ./internal/api/ ./internal/webadmin/`、`node --check internal/webadmin/assets/app.js` 与 `flutter analyze --no-pub` 通过；Flutter 全量 740 项通过，批量移动新增服务端边界、Dart API、首页组件与本地存储定向测试也通过；`git -c core.whitespace=cr-at-eol diff --check` 通过（Windows 工作树为 CRLF）。本轮没有重跑真实服务端 HTTP 冒烟，也没有做策略导入文件的真实浏览器点击。

- 2026-11-14 弱网帧龄回归：`chast-video-qos-test`、`ctest -R video_qos`、`ctest -R quic_weak_network` 与 `ctest -R audio` 通过；直接复跑纯视频、DATAGRAM 音频和可靠流音频三种 1 Mbps 弱网形态均通过，最大帧龄 2873/1956/1886 ms。仍缺真实双机、跨公网和真实声卡播放验收。

- 2026-11-14 接收端结构化失败日志：系统音频与语音的异步失败现在各输出一条稳定 JSON 事件，包含 schema/version、原因、触发条件、前后阶段，以及主机编码/传输与本地解码/播放/延迟计数快照；状态不变时不重复输出。默认写 `debugPrint`，并提供可注入 reporter，不新增远程上报。`flutter test test/receiver_status_events_test.dart` 10/10、`session_start_polling`/`session_display_streams` 4/4 与 `flutter analyze --no-pub` 通过。远程采集权限、日志留存和主机侧日志关联仍待部署侧接入。

- 2026-11-14 合并 CPU 子屏缓存：`FrameInfo::cpu_bgra` 改由有界 `CpuFramePool` 发布，只有没有下游持有旧帧时才复用缓冲区，稳定尺寸下避免 GDI CPU 子屏逐帧整帧分配；缓存满时使用临时缓冲，不会无界增长。GPU 子屏仍以纹理 `CopySubresourceRegion` 零拷贝进入画布。`chast-capture-transform-test` 与 `chast-shared-capturer-test` 直接执行全部通过，后者在真实 GDI CPU 读流上确认旧帧不可变、释放后可复用；`capture_transform`、`shared_capture`、`shared_capturer`、`desktop`、`desktop_stream`、`viewer_display_streams` 六项 CTest 6/6 通过（47.41 s）。多屏全部软件兜底的真实硬件压力仍待验收。

- 2026-11-14 PostgreSQL 真库验收：临时 PostgreSQL 13.23 与 14.25 全部通过账号/对等存储集成测试，包括新增的 v17→v18 会话活动回填；13→14 `pg_dump`/`pg_restore` 通过，账号/对等版本保持 v18/v3；空库上两个真实 `chast-server.exe` 并发冷启动通过，两个 `/v1/server` 都报告 accounts、迁移记录无重复。真库测试发现并修复录像配额 SQL 的 `unknown - unknown` 运算符歧义。证据见 [PostgreSQL 13/14 真库验收](postgres-acceptance-2026-11-14.md)。

- 2026-11-14 录制归档断点续传（工作区，不触协议）：新增活动上传会话 API，开始后返回持久偏移，PATCH 按 `X-Upload-Offset` 与精确长度追加，完成时校验完整大小、计算 SHA-256 并原子发布录制，DELETE 丢弃会话；旧单次上传继续兼容。服务端按账号目录持久化状态和分片，偏移以 `.part` 文件大小为准，重启后可继续；短写/超写/同步失败回滚到原偏移。每账号最多 8 个活动会话，每片最多 32 MiB，开始阶段预占配额，完成时事务再校验。Flutter 默认 8 MiB/片，超时或断线后查询服务端偏移恢复，完成响应丢失时用列表元数据核对，新路由不存在时回退旧服务端。存储与 HTTP 专项测试覆盖重启恢复、409 实际偏移、跨账号 404、删除、配额、分片/会话上限与下载 SHA-256；Flutter 覆盖中断重试、完成响应丢失和旧服务端回退。真实 `chast-server.exe` 回环在 1,048,576 B 首片后强杀并重启，从相同偏移续传至 2,109,497 B，下载逐字节与 SHA-256 一致，跨账号上传/成品均 404。详见 [录制归档断点续传](recording-resumable-upload-2026-11-14.md)。

- 2026-11-14 当前工作树完整回归：Go 服务端 `go test ./... -count=1` 全部包通过，`go vet ./...` 与 `go build ./...` 通过；Flutter `analyze --no-pub` 无问题，全量 `flutter test --no-pub` 763/763 通过；原生关键 CTest 扩展为 `capture_transform`、`shared_capture`、`shared_capturer`、`desktop`、`desktop_stream`、`viewer_display_streams`、`video_qos`、`quic_weak_network` 共 8/8 通过（82.11 s）。录制断点续传另以真实服务端进程、临时 SQLite 和真实文件完成强杀重启、持久偏移续传、下载字节/SHA-256 与跨账号 404 回环。PostgreSQL 集成测试已另接真实 13.23/14.25 实例通过，PostgreSQL 14.25 上的 ACL 整链也已在本机回环通过；生产 PostgreSQL 部署、生产升级和跨机 ACL 仍待环境。

## 4. 工作区与边界

- GitHub 上 `main` 的已提交协议版本为 **v73**（`5590d75`）；本分支源码已推进到 **v76**，并包含目录上传、PNG 剪贴板、临时自由标注、ACL 文档收口、账号 schema v18 会话分页、PostgreSQL 13/14 真库验收和录制归档断点续传。
- 未提交内容是一组需要保留的工作树修改和测试产物，不是“工作区干净”状态；并行同步只允许 fast-forward，不能以清理或重置工作区代替合并。
- 环境限制：本机只有单显示器、Intel GPU，无合成输入/锁屏/断网；临时服务器无 SMTP/SMS；自动化浏览器在早期原生 `confirm` 后卡死，管理台仍以静态断言与 HTTP 冒烟替代真实点击。
- 运行时文件（`chast-server-identity.key`、`chast-server-state.json`、`*.pid`、`mp4_recorder_*.mp4` 等）保持未跟踪、未提交。

## 5. 后续计划

1. **G19 设计已完成，暂不编码**：见 [G19 三项差异化能力决策](g19-decision-2026-11-14.md)。远程打印仍需可签名虚拟打印驱动，文件导出、系统 PDF 打印和截图均不满足透明打印语义；Switch Sides 保持不实施，需等主机与查看器合并为单一对等程序；AV1 4:4:4 需 AV1 Profile 1 与目标 GPU 的硬件编解码及全色度零拷贝链路。三项分别记录恢复条件和验收门槛。
2. **S20 共享密码设计已完成，等待产品取舍**：见 [S20 地址簿共享密码设计](shared-password-design-2026-11-14.md)。推荐端到端设备密钥：每条密码独立条目密钥，按查看器 X25519 公钥封装，新设备由旧设备批准或恢复包解锁；服务器只存密文。若必须由服务器管理员恢复，则改用托管密钥，但不能宣称为端到端加密。确认取舍后再实现账号同步、共享 ACL、撤销/轮换和审计。
3. **环境验收清单**（代码已具备，缺真实环境）：生产 PostgreSQL 部署/升级与 ACL 整链、真实 SMTP/SMS 发信、AD/OpenLDAP + 证书链/StartTLS、企业 IdP（Keycloak/Entra）、硬件认证器、多设备策略回执收敛、双机会话与真实中继网络。
4. **服务端候选增量（不触协议，可与并行工作流并行）**：✅ 账号目录邮箱/手机过滤与排序（2026-11-13）；✅ 会话列表分页与在线状态（2026-11-14，在线口径为最近 5 分钟有已认证请求）；✅ 审计 JSON/CSV 按分组、账号、主机、动作与时间组合过滤（2026-11-14）；✅ 策略对象 JSON 导入/导出（2026-11-14）；✅ 地址簿共享的组/Everyone 撤销审计细化（2026-11-14）；✅ 地址簿多设备原子批量移动（2026-11-14）；✅ 地址簿分组 JSON 导入/导出（2026-11-14）；✅ 录制归档大文件断点续传（2026-11-14，旧接口兼容）。下一项从剩余候选继续推进；不用低价值界面改动代替环境验收。
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
