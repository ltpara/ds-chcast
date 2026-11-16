# S01 通用客户端策略契约与实现记录

日期：2026-11-16。关联 [执行方案 S01](rustdesk-execution-plan-2026-11-16.md)。

当前状态：配置目录、存储、管理 API、设备分配解析、导入导出、主机设备签名获取、查看端账号认证获取、独立回执、代理缓存转发、管理台编辑/状态界面，以及 Windows 托盘/service 的实际消费、会话创建快照和策略设置界面已实现。查看端在托管 host lookup 中接收账号/设备/client key 绑定的随机上下文，tunnel 前固定五项配置，并在会话启动后 best-effort 提交独立回执。S01 因此为“实现完成，待部署实机验收”；服务端保存、代理缓存或自动化测试成功都不能替代真实部署验收。

## 1. 模型选择

客户端配置挂在已有命名策略上，复用 `all/group/device` 分配对象及既有优先级解析。客户端配置的修订号与设备访问控制版本独立：仅修改客户端配置不增加访问策略版本，不导致已有访问票据因配置编辑而被撤销。

配置使用完整覆盖层，单个设置形如：

```json
{
  "host.keep_awake": {"mode": "locked", "value": false},
  "viewer.tunnel_mode": {"mode": "default", "value": "relay"}
}
```

- `locked`：覆盖允许由服务端管理的本地偏好，并在本地界面解释来源。
- `default`：仅在没有显式本地偏好时提供值。客户端内建默认值不算用户显式偏好。
- 键缺失：继承本地值。空对象表示移除整份覆盖层。
- 覆盖层独立保存，不能写回用户原始偏好；撤销时恢复本地设置。
- 当前采用整份策略获胜，不把多个分配对象逐键混合。优先级从高到低；同优先级按设备、分组、全局，再按现有更新时间与 ID 规则决胜。较窄的空配置也会覆盖较宽配置。
- 手工设备访问策略只覆盖准入规则，不关闭配置分配。此区别必须在管理台说明。
- 主机与查看端按目标分别投影配置和版本；查看端配置是连接到该受管设备时的会话覆盖，不修改该查看端其他连接或服务器的全局设置。

当前受控目录不接受密码、令牌、任意命令、任意文件路径、未知键或权限提升开关。后续扩展目录需要一起增加验证、消费端实现和验收，不以保存任意 JSON 代表支持对应设置。

## 2. 首批配置目录

| 键 | 类型/范围 | 应用时机要求 | 对应现有能力 |
| --- | --- | --- | --- |
| `host.lock_after_session_end` | boolean | 下一会话 | 最后查看者退出后锁定 |
| `host.keep_awake` | boolean | 主机重启 | 主机会话电源请求 |
| `host.auto_disconnect_minutes` | integer，0–10080 | 下一会话 | 无操作断开；0 关闭 |
| `host.max_viewers` | integer，1–8 | 主机重启 | 最大并发查看者 |
| `host.carrier` | boolean | 主机重启 | TCP/WebSocket 回退监听 |
| `host.lan_discovery` | boolean | 主机/隧道重启 | 主机 LAN 发现应答 |
| `viewer.tunnel_mode` | auto/direct/relay | 下一会话 | 连接路由模式 |
| `viewer.use_websocket` | boolean | 下一会话 | WebSocket 载体 |
| `viewer.upload_recordings` | boolean | 下一会话 | 已完成录制归档意图 |
| `viewer.sync_initial_clipboard` | boolean | 下一会话 | 初始剪贴板同步 |
| `viewer.view_fit` | integer，0–1 | 下一会话 | 自适应/原始大小 |

目录只列首批，尚不表示消费者已经应用这些键。目录外的新能力（如默认仅查看）随 C01 实现后再加入。剪贴板、录制等仍必须满足设备能力、权限和必要同意，Strategy 不可绕过这些检查。网络设置不能在控制请求处理中改写正在使用的服务器身份或通道。

## 3. 已实现的管理 API

| 方法与路径 | 用途 | 鉴权 |
| --- | --- | --- |
| `GET /v1/client-strategies/catalog` | 类型、范围和应用时机目录 | 无分组限制的 admin，devices 资源 |
| `GET /v1/policies/{id}/client-strategy` | 读取命名策略配置；未设置为 revision 0、settings {} | 同上 |
| `PUT /v1/policies/{id}/client-strategy` | 全量保存，要求 `if_revision` 与 `settings` | 同上 |
| `GET /v1/device-strategies/{peer}?target=host` | 预览该设备获胜配置；target 也可为 viewer | consoleRead 起，devices 资源及设备分组作用域 |
| `GET /v1/hosts/{peer}/client-strategy` | 主机获取当前完整配置，包含显式撤销 | 主机凭据及设备私钥签名、时间窗；禁用设备拒绝 |
| `POST /v1/hosts/{peer}/client-strategy/receipt` | 上报主机逐键应用结果 | 同上；独立签名域，不能代替查看端回执 |

预览要求设备已注册。未知设备返回 404，非法目标/键/类型/范围返回 400，过期修订返回 409。重复 JSON 设置键和重复设置字段会被拒绝。配置最多 16 KiB，版本不超过 JavaScript 精确整数范围。审计记录对象和修订号，不记录配置值。

设备预览增加 `receipt`，没有当前版本回报时为 null。回执绑定 revision、digest、sequence 和已验证的设备身份公钥；同一配置的后续应用结果使用递增 sequence，防止旧“待重启”覆盖新结果。完全相同的重复回执幂等且不重复产生审计；同序列不同结果、旧版本、缺失逐键结果或汇总状态不一致均拒绝。更换设备身份密钥后旧结果不可见，不能继承旧设备的应用状态。

当前汇总状态为 applied / pending_session / pending_restart / unsupported / failed。非空配置须逐键完整报告，汇总取最严重状态；空配置也必须明确回报撤销结果，撤销本身可能需要重启或失败。

预留 S03 操作划分：读取配置/目录 `strategy.read`、修改 `strategy.write`、设备预览 `strategy.preview`。本轮仍使用现有角色鉴权；完整 Admin Role 未实现。

### 配置版本与快照

`policy_client_strategies` 保存作者配置和修订号。保存须提供旧修订号；相同内容不增加版本；清空保留版本记录。普通访问策略编辑和旧格式导入不会改动配置。

`device_client_strategies` 按设备 ID 与目标保存递增版本、配置来源、配置和摘要。配置撤销产生空快照，重新分配继续递增，不能重置为 1。快照读取先读取目标版本，再解析源，写入使用条件更新，防止其他实例已发布的版本被晚到写入覆盖。竞争时返回 409，交付端后续需重新获取。

摘要为目标类型、NUL 分隔符及规范化设置 JSON 的 SHA-256。JSON 使用排序键和严格 bool/int/enum 值；规范化后空配置为 `{}`。摘要用于绑定回执，不代替传输认证。

### 导入导出

- 没有客户端配置历史的导出保持 `chast-policy-objects` v1，兼容现有使用。
- 存在配置历史时导出 v2，每个对象必须带 `client_settings`，包括空对象。
- v1 导入保持现有配置；v2 导入替换配置。配置与访问策略在同一事务中导入，非法设置不会产生部分对象。
- 仅修改客户端配置的导入不增加访问策略版本；结果计入 updated。
- 旧服务器应拒绝 v2，不降级导出为丢失配置的 v1。

## 4. 客户端交付进展及剩余要求

### 已实现：主机代理传输和管理台

- `chast-tunnel` 使用设备密钥签名拉取 host 配置，每 15 秒同步；使用既有 `--policy-state` 路径时启动独立循环，不阻塞心跳和注册。一次获取及上报固定在同一服务端点。
- 代理把配置原子保存到 `<policy-state>.client-strategy`，与访问权限标记隔离；状态包含协议格式版本、服务器 URL/固定公钥、主机公钥和完整配置快照。作用域摘要绑定服务器身份、设备 ID 和设备身份公钥。
- 网络/鉴权失败不撤销缓存；拒绝错误设备、错误目标、摘要不符、旧版本及同版本不同内容。重启后继续使用磁盘版本判断，显式空快照才撤销。
- 代理只转发 `<policy-state>.client-strategy.ack` 中与当前作用域、版本、摘要匹配且逐键结果完整的回执。原生消费端负责持久化 sequence；代理本身不会凭收到配置生成 applied。
- 管理台“客户端设置”提供类型化编辑、管理方式、应用时机和版本冲突提示；“设置状态”分别展示 host/viewer 的目标版本与当前回报。没有回报时明确不能确认生效。
- 本地浏览器实测完成保存、关闭后重新读取、并发修改冲突提示及输入保留。没有用测试回执冒充真实主机执行。

### 已实现：原生主机生命周期、回执与设置界面

- `include/chast/host_client_strategy.h` / `src/host/host_client_strategy.cc` 提供独立的缓存读取和完整覆盖层计算。按代理的同一算法，从本地主机密钥派生 Ed25519 公钥，校验服务器 URL、公钥 pin、设备 ID、公钥与 scope；不从缓存自行接受新的信任根。
- 严格校验 64 KiB 文件上限、嵌套深度、重复字段（含转义后重复）、UTF-8/Unicode、受控类型与范围、16 KiB 配置上限、规范化 SHA-256 摘要及修订顺序。错误、缺失文件和旧版本均不修改调用方已经接受的状态。未知 `host.*` 标量设置保留为 unsupported 候选，不改写已知配置；托盘/service 生命周期已接入真实回执，启动或应用失败会按逐键结果上报。
- 六项设置的内建默认值不再由普通保存操作自动写入 INI；有效的显式配置行和用户实际修改保留本地意图。`default` 仅覆盖无显式本地偏好的值，`locked` 覆盖本地偏好；空配置从本地值重新生成完整运行时设置。运行时副本带禁止序列化标记，不能通过保存接口写回本地配置。
- 旧 INI 的实际配置行保守地视为显式偏好，即使值等于内建默认值；旧保存器是否曾自动写入该值已无法可靠推断。要让某项恢复继承，可在主机停止后删除对应本地配置行，再重新启动。旧版本若重新保存完整默认值，会重新将它们变为本地偏好；升级验收须覆盖此边界。
- 新增 `lan_discovery` 本地字段；`lan_discovery=0` 在隧道下次启动时传入 `--lan-discovery=false`，使用 Go 布尔参数的等号形式，避免错误地保留开启状态或停止解析后续参数。主机设置界面已提供 LAN 发现入口，但 C01 的完整验收仍包含主动扫描、真实 LAN 环境和查看端交互。
- Go 与 C++ 共用 `tests/data/host_client_strategy.json`，验证真实 Go 输出的 scope、公钥、配置摘要和 Unicode 元数据，避免两端各自模拟相同错误。
- 托盘和 service worker 在启动、策略刷新及会话创建时消费有效设置。`keep_awake`、`max_viewers`、`carrier`、`lan_discovery` 的变化会重启主机或 tunnel；锁屏和空闲断开使用会话创建快照，当前会话不因后续策略变化被改写。
- 新增 `<policy_state>.client-strategy.ack` 原子回执。回执按 revision、digest、scope 和 sequence 隔离，明确启动失败按逐键 `failed` 上报；未知合法键保留在快照中，但阻止新查看者并逐键回报 `unsupported`。
- “客户端策略”设置页展示来源、policy ID、revision、六项策略的 local/managed default/locked 状态和有效值，并提供 LAN 发现入口及本地显式偏好的“恢复继承”操作。运行时覆盖层不能写回 `host.ini`。

### 已实现：查看端交付、会话应用和独立回执

- 账号认证的托管 host lookup 返回按账号、设备和 client key 绑定的独立随机 delivery context；每次 lookup 使用新的明文随机值，账号库分别保存每个有效上下文 10 分钟，新 lookup 不会替换仍在使用的旧交付。未认证、应急访问或格式异常的 lookup 不返回可冒充的查看端策略。
- 查看端严格校验 delivery 的来源设备、target、revision、digest 和五项设置，拒绝旧响应或同版本不同内容。`tunnel_mode`、`websocket`、`upload_recordings`、`sync_initial_clipboard`、`view_fit` 在会话开始时固定成快照；续票刷新可以更新策略是否仍可用，但不替换当前 live session 的路由快照。
- 会话身份边界包含账号、服务器、peer 和连接设置。边界改变时推进 generation、停止旧 tunnel/session，再按新身份重新 lookup；旧 lookup 和 token renewal 完成后再次核对 generation，不能越过新登录或新目标。
- tunnel 创建前校验账号服务器与目标服务器一致，避免使用错误账号 namespace。路由、WebSocket、录制上传、初始剪贴板和视图适配均读取固定快照，而不是在会话中途直接读取可变全局设置。
- 会话启动后通过 `POST /v1/account/device-strategies/{peer}/receipt` 上报逐键结果，回执绑定 delivery context、client public key、revision/digest 和账号/设备身份，不能复用设备级结果。回执失败是可见的 best-effort 失败，不拆已经建立的会话，也不显示 applied。
- 初始剪贴板在 UI 切换时更新下一连接使用的有效快照；策略锁定时只记录本地偏好选择，不绕过锁定值。

### 仍需部署验收

1. 在两台 Windows 实机、两个查看端和不同权限账号下验证 host/viewer 五项设置、锁定、建议值、撤回、未知键、断网重连和管理台逐键状态。
2. 在隔离 PostgreSQL 实例执行 schema 19→22、重复迁移、备份恢复和条件测试，并验证旧二进制降级边界。
3. 验证真实托盘/service/tunnel 重启、异常退出和 tunnel 明确关闭路径；只有实际消费成功才能显示 applied。
4. 在中文/英文及完整机器设置编辑器中做人工布局验收，确认运行时覆盖层从不写回本地偏好。
5. 完成管理台浏览器端到端验收，明确未回报的查看端不能显示为已应用。

这些项目未完成前，S01 不能标记为生产验收完成。

## 5. 数据库升级和恢复

账号库 schema 从 19 增至 22：v20 新增配置与目标快照两个表，v21 新增应用回执表，v22 新增查看端 delivery context 与独立回执支持，不修改原访问策略、账号和地址簿数据。迁移可重复执行。SQLite 已通过新建、模拟已有数据迁移和重启测试；PostgreSQL 条件测试已扩展到回执，但当前未配置 `CHAST_TEST_POSTGRES_DSN`，未执行。

升级前使用项目已有数据库备份流程保存完整备份。迁移为向前迁移，旧二进制会拒绝 schema 22；回退必须使用兼容 schema 22 的二进制或恢复升级前备份，不能直接启动旧版本写新库。恢复备份会丢失备份后的写入，需在部署计划中约定恢复点。

## 6. 本轮验证

- `go test ./internal/strategy ./internal/account ./internal/api -count=1 -timeout 10m`：三个包通过。
- 补充跨实例竞争和跨组权限测试后的定向回归通过；PostgreSQL 专项明确跳过。
- `go vet ./...`：通过。
- 覆盖：配置类型/范围与歧义 JSON、修订冲突、访问权限版本隔离、分配优先级、目标隔离、撤销/重新分配、重启持久化、已有数据迁移、权限/资源/分组隔离、v1 保留与 v2 恢复、导入失败原子性。
- 最后补齐 PostgreSQL 迁移注册与 BIGINT 字段后，策略、迁移、导入导出及权限定向回归再次通过，Go 全项目静态检查再次通过；该迁移注册测试不替代 PostgreSQL 实例验证。
- `git diff --check` 及新增文档内部链接检查通过。
- 日志：`build/strategy-server-tests-20261116.log`、`build/strategy-focused-tests-20261116.log`、`build/strategy-final-focused-20261116.log`、`build/strategy-go-vet-20261116.log`。

### 主机交付与管理台增量验证

- account / api / agent / strategy / identity / webadmin 六个包全量通过，分别用时约 49.3 / 75.2 / 133.6 / 0.3 / 0.3 / 0.4 秒。
- 配置回执绑定身份公钥后的最后定向回归及 Go 全项目静态检查另行留存，见 `build/strategy-delivery-final-20261116.log` 与 `build/strategy-delivery-vet-20261116.log`。
- 代理测试覆盖签名获取、精确原生回执转发、服务器/设备作用域、网络失败保留、回退拒绝、磁盘重启与显式撤销；API 测试覆盖伪造/跨设备/过期签名、报文篡改、禁用设备、重复回执与审计幂等。
- 本地管理台使用独立临时 SQLite 数据库和仅回环监听；浏览器实测新建策略、保存、重新加载，以及另一请求修改后的 409 冲突提示；截图位于 `build/strategy-ui-review/conflict.png`。未触碰生产数据库。
- `node --check server/internal/webadmin/assets/app.js` 通过。
- 管理台认证复查发现 F4：跨账号重试、退出后保存旧刷新结果、并发重复刷新。已修复原账号绑定及刷新合并，`node --test server/internal/webadmin/app_session_test.cjs` 三项回归通过；修复前失败日志与修复后日志分别为 `build/strategy-admin-session-before-20261116.log`、`build/strategy-admin-session-after-20261116.log`。
- `go build ./cmd/chast-server ./cmd/chast-tunnel`、最后的 Go 静态检查及 `git diff --check` 通过。浏览器测试完成后已停止隔离服务，临时数据库与截图保留在 `build/strategy-ui-review/`。

以上原生主机生命周期验证不能替代真实查看端策略、PostgreSQL 实例或多实例生产验收。

### 查看端闭环增量验证

- `flutter test test/session_viewer_strategy_test.dart`：5/5 通过。
- `flutter test test/l10n_test.dart`：7/7 通过。
- `flutter test test/session_start_polling_test.dart`：2/2 通过。
- `flutter test --reporter expanded`：**814/814 通过**，日志为 `build/strategy-viewer-full-20261116-r2.log`。
- `flutter analyze`：**No issues found**。
- 回归覆盖身份变化先停止旧 tunnel、回执失败不拆 live session、续票不替换当前路由快照、初始剪贴板更新下一连接快照、旧 lookup/renewal 不能越过身份代次，以及账号服务器与目标服务器不一致时拒绝 tunnel。
- 这些结果证明当前工作区的自动化和接口闭环，不证明 PostgreSQL 实库、真实双机、主机重启、管理台端到端或生产部署通过。

### 原生配置基础增量验证

- 原生 Release 构建：`chast-host-client-strategy-test`、`chast-host-settings-test`、`chast-host-tunnel-test`、`chast-host` 成功；服务实际目标 `chast-service` 另行构建成功。第一次构建命令末尾误用了不存在的 `chast-host-worker` 目标，导致命令整体退出 1；已核对服务目标并补建，不将该次命令写成全量构建成功。
- `ctest --test-dir build -C Release -R '^(host_settings|host_tunnel|host_client_strategy)$' --output-on-failure`：3/3 通过，5.80 秒。覆盖显式默认值保留、旧文件迁移、服务设置合并、LAN 启动参数、身份绑定、解析边界、撤回/回退、未知键与运行时副本不可持久化。
- `go test ./internal/agent -run ClientStrategy -count=1 -timeout 2m`：通过，4.353 秒；包含新增跨语言共用样本验证。
- 日志：`build/strategy-native-cache-build-20261116.log`、`build/strategy-native-service-build-20261116.log`、`build/strategy-native-cache-test-20261116.log`、`build/strategy-native-cache-go-20261116.log`。构建仍有原有变量遮蔽/运行库链接警告，新纯逻辑测试也报告未使用的延迟加载项警告；没有将这些记录为新的运行时验证。
- 这批验证证明解析与覆盖规则；主机自动消费和查看端应用已在本轮接通，真实部署验收仍待完成。

### 原生生命周期、回执与设置界面增量验证

- Release 定向构建 `chast-host`、`chast-service`、`chast-host-client-strategy-test`、`chast-lock-after-session-test` 成功；仅保留项目既有的运行库冲突警告。
- `ctest --test-dir build -C Release --output-on-failure -R '^(host_settings|host_client_strategy|host_tunnel|lock_after_session)$'`：4/4 通过，总耗时 6.31 秒。覆盖显式本地偏好、策略覆盖与撤销、身份/摘要/修订校验、局域网启动参数、逐键回执、revision 隔离、sequence 持久递增、未知键 `unsupported`、明确启动失败 `failed` 与会话快照。
- 这批验证证明主机侧消费已接通；查看端交付/应用/回执随后完成，当前进入真实端到端验收阶段。
- 最新补丁后再次执行 `ctest --test-dir build -C Release --output-on-failure -R '^(host_settings|host_client_strategy|host_tunnel|lock_after_session|host_multi)$'`：5/5 通过，总耗时 54.22 秒。新增 `host_multi` 覆盖策略不可用时拒绝新连接、解除阻断后允许后续连接，验证准入开关已由监听器与多查看器 worker 共享。
