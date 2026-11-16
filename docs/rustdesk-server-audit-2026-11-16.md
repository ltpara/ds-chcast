# 服务器端与 RustDesk 功能差异审核（2026-11-16）

## 1. 审核目标与范围

本次只审核 `ds-chcast` 的服务器端能力，目标是判断其功能是否已经与 RustDesk OSS 和 RustDesk Server Pro 对齐。发射端、接收端、画面质量和客户端交互不在本次范围内。

对比基线：

- 本项目：`D:\ds-chcast`
- 本项目分支：`codex/backup-pending-features-2026-11-14`
- RustDesk：`D:\rustdesk`
- RustDesk 提交：`55786aa`
- 审核日期：2026-11-16

本地 RustDesk 仓库包含客户端、协议定义和客户端对 Server Pro API 的调用，但不包含 RustDesk Server Pro 的完整服务端源码。因此，本次对 Pro 功能的判断同时依据：

1. RustDesk 客户端协议和 API 调用；
2. RustDesk 官方 Server Pro 文档；
3. `ds-chcast` 的 Go 服务端、管理 API、数据库迁移和相关测试。

## 2. 总体结论

`ds-chcast` 的服务器通信主干已经基本覆盖 RustDesk OSS 的核心能力，在部分中继调度和企业安全能力上更丰富，包括会合、注册、NAT 探测、打洞、中继、账号认证、设备准入、地址簿共享、审计、录制上传和运维监控。

当前仍不能认定服务器端已经完整对齐 RustDesk Server Pro。完整客户端 Strategy、活动远控会话强制断开和自定义 Admin Role 的服务端主体已经接通；管理台角色接入与浏览器验收仍待完成。其余差距集中在地址簿密码同步、独立命名共享地址簿、Control Role 三态覆盖、Alarm 日志、用户邀请和邮件偏好，以及定制客户端生成。

如果目标还包括让官方 RustDesk 客户端直接连接本项目服务器，则存在更高优先级的协议兼容差距：本项目使用自有 HTTP/JSON、wire 和 QUIC 协议，没有实现 RustDesk protobuf hbbs/hbbr 线协议。

## 3. 已覆盖或具有优势的能力

| 功能域 | 当前能力 | 结论 |
| --- | --- | --- |
| 会合与注册 | 主机注册、查询、身份管理、NAT 探测、UDP/TCP 打洞、IPv4/IPv6、LAN 地址处理 | 已覆盖核心路径 |
| 中继 | UDP 中继、TCP/WebSocket 载体、独立中继节点、区域优先、健康检查和故障回退 | 已覆盖，调度能力较完整 |
| 账号认证 | 密码、TOTP、恢复码、邮件/短信验证码、WebAuthn、OIDC、LDAP 首次登录建号 | 已覆盖主要企业认证方式 |
| 地址簿基础 | 标签、备注、分组、嵌套、排序、回收站、增量同步 | 已覆盖 |
| 地址簿授权 | 可共享给用户、账号组和 Everyone，支持 read/write/control 及分组范围 | 已覆盖较细授权 |
| 设备访问控制 | 账号组、设备组、设备准入、签名访问票据、权限上限、撤权推送和应急访问 | 已覆盖核心 ACL |
| 审计与留存 | 操作审计、连接记录、文件传输记录、CSV 导出和按天留存 | 已覆盖主要审计能力 |
| 录制管理 | 上传、断点续传、配额、管理端下载和删除 | 已覆盖 |
| 运维 | API Token、Prometheus、TLS/mTLS、ACME、SQLite/PostgreSQL、软件发布和签名校验 | 已覆盖 |

主要实现入口：

- API 路由：`server/internal/api/api.go:286`
- 区域中继选择：`server/internal/relaypool/pool.go:302`
- 地址簿共享目标：`server/internal/account/store.go:3515`
- 策略对象：`server/internal/account/policy_objects.go:50`
- 设备准入：`server/internal/account/device_policy.go:13`
- 控制台授权：`server/internal/api/console.go:9`
- 审计保留：`server/internal/account/retention.go:9`

RustDesk Pro 官方 LDAP 文档明确不支持 LDAP 组同步，因此本项目当前没有 LDAP 组同步不计为本次差距。

## 4. 明确功能差距

### 4.1 完整 Strategy 客户端设置同步（P1）

RustDesk 客户端心跳可以接收 Strategy，并将任意 `config_options` 应用到客户端设置。相关代码：

- `D:\rustdesk\src\hbbs_http\sync.rs:45-50`
- `D:\rustdesk\src\hbbs_http\sync.rs:243-275`
- `D:\rustdesk\src\hbbs_http\sync.rs:295-335`

本项目虽然已经实现命名策略、`all/group/device` 分配、优先级、版本、回执和设备准入，但策略对象只包含：

- `open/restricted` 模式；
- 允许用户和允许账号组；
- 32 位会话权限上限。

证据：`server/internal/account/policy_objects.go:54-64`。

因此，当前策略系统属于访问策略，而不是 RustDesk Pro 的完整客户端配置 Strategy。尚不能集中下发客户端安全、行为、网络和界面等通用设置。

### 4.2 自定义 Admin Role 权限矩阵（P1）

RustDesk Pro 支持自定义管理角色，并可按全局、单对象和组范围细分用户、设备、组、策略、审计和定制客户端等管理权限。

服务端已经实现稳定权限键、命名 Admin Role、全局/组/对象作用域绑定、角色和绑定 API、审计及角色提权防护，提交为 `d65d62c`。现有固定角色继续映射到同一权限目录，保持原语义。

主要入口：

- `server/internal/account/admin_roles.go`
- `server/internal/account/role_bindings.go`
- `server/internal/api/admin_roles.go`
- `server/internal/api/console.go`

剩余差距集中在管理台：`app.js` 仍按固定角色判断，尚未保存和消费 `/v1/account` 的 `permissions`，也没有角色创建、权限多选和绑定界面。因此当前不能把本项写成完全完成，仍需管理台浏览器 E2E、PostgreSQL 实库和完整越权回归。

### 4.3 活动远控会话查询和强制断开（P1）

RustDesk 客户端心跳会上报连接编号，服务端响应可返回 `disconnect` 编号列表，客户端收到后断开对应连接：

- `D:\rustdesk\src\hbbs_http\sync.rs:238-260`

本项目当前的连接对象是会话结束后提交并持久化的审计记录，包含开始、结束时间、路由和结果：

- `server/internal/account/connection.go:57-78`
- `server/internal/api/api.go:361-363`

未发现管理台用于查询当前活动远控会话并对某条连接发送强制断开命令的 API 或控制通道。`/v1/account/sessions` 管理的是网页登录会话，不是远控连接。

### 4.4 地址簿密码同步（P1/P2）

RustDesk 地址簿条目支持密码字段，Strategy 同步也包含预设地址簿密码：

- `D:\rustdesk\flutter\lib\models\ab_model.dart:374-390`
- `D:\rustdesk\src\hbbs_http\sync.rs:148-154`

本项目 `address_book` 表只有设备 ID、名称、标签、备注、分组、排序、修订和删除状态，没有密码字段：

- `server/internal/account/migrate.go:568-579`

当前保存密码的能力位于客户端本地，不能跨设备同步个人地址簿密码，也不能提供共享簿默认密码或条目级密码。实现该能力前需要先确定端到端加密、密钥恢复和共享授权模型，不能直接在服务器保存明文。

### 4.5 多个独立命名共享地址簿（P2）

RustDesk Pro 客户端按地址簿 profile 管理多个共享簿，每个 profile 有独立标识、名称、所有者和权限：

- `D:\rustdesk\flutter\lib\models\ab_model.dart:42-47`
- `D:\rustdesk\flutter\lib\models\ab_model.dart:159-173`
- `D:\rustdesk\flutter\lib\models\ab_model.dart:295-350`

本项目的 `SharedBook` 表示“另一个用户的个人地址簿”，共享范围可以是整个个人簿或其中一个分组：

- `server/internal/account/store.go:325-344`

现有模型的授权能力较强，但不能创建多个脱离个人簿、各自独立命名和管理的共享地址簿，数据模型不等价。

### 4.6 Control Role 三态覆盖（P2）

RustDesk Pro Control Role 可对每项会话权限指定三种状态：跟随客户端设置、强制启用、强制禁用，并按控制用户分配角色。

本项目使用账号级和设备级 `permission_ceiling` 进行位掩码求交，只能限制最终权限：

- `server/internal/account/device_policy.go:538-559`
- `server/internal/api/access_tickets.go:44-58`

该模型可以强制禁用某项能力，但不能表达服务端强制启用，也没有独立的命名 Control Role 对象。

### 4.7 Alarm 告警分类（P2）

RustDesk Pro 管理台区分 Connection、File、Console 和 Alarm 等日志类别。

本项目已经具备审计、连接和文件传输记录，也具备登录锁定、反滥用和中继阻断机制，但未发现独立持久化 Alarm 事件模型、查询 API 和管理页面。因此当前只能通过普通审计或外部指标观察安全事件，未达到 RustDesk Pro 的告警日志体验。

### 4.8 用户邀请、邮箱验证状态和告警邮件偏好（P2）

RustDesk 客户端用户结构包含：

- `email_verification`
- `email_alarm_notification`
- 登录设备白名单

证据：`D:\rustdesk\src\hbbs_http\account.rs:53-69`。

本项目支持邮箱登录验证码、密码找回和管理员发送重置邮件，但未发现：

- 用户邀请流程；
- 持久化的用户邮箱已验证状态；
- 用户告警邮件开关；
- 登录设备白名单。

OIDC 会读取上游 `email_verified` 声明，并只在声明可信时保存邮箱，但这不是本地邮箱验证和用户偏好功能：

- `server/internal/oidc/oidc.go:116`
- `server/internal/account/oidc.go:59`

### 4.9 定制客户端生成与品牌管理（P2）

RustDesk Server Pro 管理台可以维护定制客户端配置，并提供定制构建或下载流程。

本项目具备软件托管、更新发布、签名校验、安装参数和配置推送能力，但没有用于品牌信息、图标、默认配置和构建产物的定制客户端生成管理台。

### 4.10 hbbs TCP HTTP 代理回退（P2）

RustDesk 客户端在 HTTP API 无法直接到达时，可以通过与 hbbs 的安全 TCP 连接发送 protobuf `HttpProxyRequest`：

- `D:\rustdesk\libs\hbb_common\protos\rendezvous.proto:262-273`
- `D:\rustdesk\src\common.rs:1445-1503`

本项目未发现对应的通用 HTTP 代理隧道。这不会阻止普通部署工作，但在只允许 hbbs 端口、HTTP API 被代理或防火墙阻断的环境中，RustDesk 的可达性更强。

### 4.11 RustDesk hbbs/hbbr 协议兼容（条件性 P0）

本项目按 RustDesk 的功能和连接节奏实现了会合、中继和打洞，但使用自有协议，没有实现 RustDesk `rendezvous.proto` 的 hbbs/hbbr 线协议。

因此：

- 如果目标是功能和性能对齐，该项属于架构差异；
- 如果目标包含官方 RustDesk 客户端无需修改即可连接本项目服务器，该项是 P0 阻断项。

## 5. 实现方式不同但不直接判定缺失

RustDesk Pro 可以结合 GeoLocation/MaxMind 信息选择较近的中继节点。本项目使用运维配置的 CIDR 到地点标签，以及中继区域标签进行区域优先和健康回退：

- `server/cmd/chast-server/main.go:80`
- `server/cmd/chast-server/main.go:170`
- `server/internal/relaypool/pool.go:345-377`

两者都能实现区域优先，但本项目不是基于自动地理距离计算。是否需要补齐 MaxMind 自动选路，应由目标部署规模和运维方式决定。

## 6. 生产验收缺口

以下项目属于生产验收或部署可靠性问题，不应误记为代码功能缺失：

- 控制面高可用和自动故障切换尚未完成生产验收；
- 多实例邮件、短信限流仍需共享状态验证；
- 多实例共享录制目录需要外部租约或对象存储协调；
- 跨区域、真实复杂 NAT、代理/CDN 和大规模容量仍需环境测试。

相关记录：

- `docs/rustdesk-overall-audit-2026-11-15.md`
- `docs/postgres-acceptance-2026-11-14.md`

## 7. 建议实施顺序

1. **P1：补齐通用 Strategy 数据模型和客户端同步通道。** 在现有访问策略之外增加类型化或受控键集合的客户端配置载荷、优先级、版本、回执和失败可见性。
2. **P1：实现活动连接目录与强制断开。** 让主机心跳上报活动连接标识，由管理端授权后向指定主机推送断开命令，并完整记录审计。
3. **P1：完成自定义 Admin Role 的管理台接入。** 服务端稳定权限键、命名角色、作用域和人类账号分配已完成；下一步接入管理台权限渲染、角色与绑定界面，并执行浏览器 E2E。
4. **P1/P2：设计地址簿密码端到端加密。** 先完成威胁模型、密钥派生、共享密钥封装和恢复设计，再扩展服务端存储。
5. **P2：增加独立共享地址簿实体。** 将共享簿从“用户个人簿视图”提升为独立资源，并保留现有分组授权能力。
6. **P2：补齐 Control Role、Alarm、用户邀请和邮件偏好。** 这些能力可以复用现有权限位、审计和邮件基础设施。
7. **按产品目标决定协议兼容。** 若必须支持官方 RustDesk 客户端，应单独立项实现和维护 hbbs/hbbr 协议兼容层。

## 8. 审核结论

服务器端已经具备可用且较完整的远控基础设施，RustDesk OSS 的核心会合和中继能力没有发现明显功能断层。当前差距主要位于 RustDesk Server Pro 的集中管理和企业治理功能。

在完成 S03 管理台接入与外部验收、地址簿密码与独立共享簿之前，不应将服务器端状态描述为“已与 RustDesk Pro 完全对齐”。
