# 当前状态与后续计划（2026-11-09，检查点 2）

本文件记录 `codex/development-checkpoint-2026-11-09` 分支在 v61 阶段的当前状态和下一步开发顺序，作为 [开发检查点](development-checkpoint-2026-11-09.md) 之后的补充入口。本文件随本次提交保存；本机 `build/` 日志、验收程序和临时配置不进入仓库。

## 1. 基线

- 仓库：`ltpara/ds-chcast`；分支：`codex/development-checkpoint-2026-11-09`。
- 上一检查点提交：`90ace27`（`chast: save host development checkpoint and VP9 v61 progress`）。
- 应用媒体/控制协议：**v61**；当前不承诺与 v60 或 RustDesk 线协议互通。
- RustDesk 行为对照固定于本地参考提交 `7423dced37845c48093b6f71bd82e4a16cc8c79d`；只参考行为与字段语义，不引入其代码或依赖。
- 本次提交范围：工作区累计的 v61 收尾、静止桌面刷新、按设备会话偏好、进程级剪贴板监听、会合服务角色密钥（G01），以及对应文档、原生测试、Go 测试和 Dart 测试。`build-review-clean/`、`server/chast-setup.exe`、`build/` 下的本机证据与构建产物不提交。

## 2. 已完成并进入生产路径

| 能力 | 状态 | 证据 | 边界 |
| --- | --- | --- | --- |
| v61 VP9 / 4:4:4 收尾（P0-1～P0-4） | 实现并完成本机验收 | 原生回归 8/8、Release 全量 ctest 64/64；真实 DXGI → VP9 → QUIC → GPU → 双 Flutter 窗口；1080p 六轮性能测量；打包 EXE 白名单 | 单机回环、单显示器、本机核显；真实 HDR、多显示器/竖屏、独立显卡、安装/升级/卸载流程未复验 |
| 静止桌面刷新对齐 RustDesk `static_refresh.rs` | 已接入 | `desktop_stream` 408–410 项、`host_parity` 976–977 项连续 3 次 0 失败；真实 Intel GPU 双查看器 92 项 0 失败 | 未做跨机画质/功耗对比 |
| 按设备会话偏好（RustDesk `PeerConfig`） | 已接入 | `stream_prefs` / `view_prefs` 26 项、会话窗口与显示流 8 项、`flutter analyze`、Release 构建 | 未做真实双主机人工切换复核 |
| 进程级剪贴板监听（G09 的 C27 部分） | 已接入 | 私有窗口站真实 `CF_UNICODETEXT` 测试 9 项、`clipboard_async`、`host_parity` 976 项 0 失败 | 服务模式/非交互窗口站、多窗口/多主机复制的真实 GUI 复核未做 |
| 会合服务角色密钥分离（G01） | 已接入 | Go `go test ./...` 全绿；Flutter 684 项通过；`chast-host-settings-test`、`chast-host-tunnel-test` 通过；真实 `chast-server` 回环完成三密钥正/负向验收 | 公网 NAT、真实反向代理、旧单密钥部署迁移、密钥分发与轮换未验证 |
| 大目录分页（G05）、单向剪贴板/文件（G06）、AV1 录制（G07） | 已完成 | 见 [RustDesk 全量对照](rustdesk-full-parity-2026-11-07.md) 与检查点文档 | 真实素材和跨网吞吐仍按各自模块边界待复验 |

## 3. 当前协议与关键实现位置

- 协议：`include/chast/protocol.h` / `src/net/protocol.cc`，`kProtocolVersion = 61`；Hello 已含能力、codec 偏好、色度和各权限位。
- 主机认证与权限合成：`src/host/host_security.cc`、`include/chast/host_security.h`；目前只有主机本地密码、审批、TOTP、可信设备、白名单和权限位裁剪。
- 会合与票据签发入口：`server/internal/api/api.go`；查看器 lookup 经 `server/internal/agent` 的 `chast-tunnel viewer` 完成，Dart 侧为 `app/lib/tunnel_agent.dart`、`app/lib/server_api.dart`。
- 账号与组：`server/internal/account/store.go`、`groups.go`、`migrate.go`；当前 schema 版本 9，账号组只服务控制台范围，**尚未形成连接 ACL**。
- 录制：主机入站自动录制已完成；**出站查看器自动录制、容量/保留/失败提示仍缺**。
- 键盘：已有物理映射与本地 IME 两种模式；**RustDesk 的 Translate / Legacy 选择未实现**。

## 4. 后续计划

### 第一优先：G02 连接 ACL（企业准入前必做）

目标：账号/组 → 设备策略，禁用设备或账号、移组、过期票据均能拒绝；直连、打洞和经服务器路径都明确受管，不允许回退绕过。

计划分片：

1. 账号存储迁移 v10（SQLite 与 Postgres 同步）：设备访问策略 `open` / `restricted`、允许的用户/组、可选权限掩码；管理 API 的 GET/PUT 与正负向测试。
2. lookup 身份解析：管理 key > link key > 账号 Bearer token；restricted 主机拒绝 link-key 调用者，管理 key 可旁路；返回服务端签发的短期访问票据。
3. 票据采用 HMAC-SHA256，key 使用主机已知的 `host_key`，域分离为 `chast-access-ticket-v1`；票据绑定主机 ID、主体、权限掩码、签发/过期时间与 nonce，base64url 编码；主机离线可验签。
4. 协议 v62：Hello 增加 `access_ticket`；主机设置增加 `require_access_ticket`；`HostSecurity` 在本地密码/审批之前校验票据并按票据裁剪权限；缺失、过期、错主机或错误签名一律拒绝。直连和经服务器/中继路径都必须在主机侧受同一规则约束。
5. 查看器链路：Go agent 把账号 token 与票据传给 Dart `TunnelAgent`，再进入原生 `ViewerEngine` 的 ClientHello；Flutter 侧不缓存票据到磁盘。
6. 验收：禁用账号、禁用设备、移组、过期票据、错主机、无票据在直连/打洞/中继三种路径下均被拒绝；open 主机行为不变；管理旁路可用；真实回环 + Go/Dart/原生回归；更新文档。

完成标准对应 [RustDesk 全量对照](rustdesk-full-parity-2026-11-07.md) 的 G02 行：不允许通过直连地址或旧 link key 回退绕过。

### 第二优先：G03 账号级会话权限票据

在 G02 的主体/票据框架上增加服务端签名的权限凭证：绑定用户、主机和有效期，主机独立验证，按明确规则与本地同意合成，覆盖敏感操作和撤权；不要求中继解密桌面数据。与 G02 分开验收，避免把“能连”误当成“能操作”。

### 第三优先：G04 集中策略

策略对象、分配范围、优先级、版本、同步确认与失败可见；明确用户设置和策略冲突时的行为。先做可验证的单机/单组策略下发，再扩展多组优先级。

### 之后按顺序

1. G09 剩余项：多窗口/多主机剪贴板真实 GUI 复核。
2. G10 键盘 Translate / Legacy：保留 Map 默认，补 Translate 并验证非 US 布局、AltGr、死键、Unicode/IME 与快捷键混合。
3. G11 自动录制：出站查看器自动录制、存储容量/失败提示/保留策略；集中留档由受管主机负责并报告失败。
4. G12 管理权限颗粒度：资源/动作 scope、组范围、独立机器令牌的创建/到期/撤销/审计。
5. G13 就近中继、G14 LDAP：按部署需求单独立项，不用合成/单机结果替代真实多地区或目录服务验收。

### 环境相关验收（不是代码缺口）

真实 UAC、跨管理员用户、多用户 RDP、多物理屏/混合 DPI/多 GPU、物理摄像头、真实 HDR、Explorer 粘贴、其他 IME、跨公网 NAT/IPv6/代理/中继、弱网和不同显卡故障恢复，仍需在对应设备或网络环境执行；测试前记录设备、步骤、预期和证据边界。

## 5. 协作与验证约定

1. 文档分开记录“已实现”“已构建”“自动化通过”“真实链路通过”“待人工验证”；模拟媒体、故障注入和 dry-run 只证明其覆盖的边界。
2. 协议字段只增量添加；语义变化升版本，并同步两端解析、非法输入处理和文档。
3. 第三方依赖通过 vcpkg 管理并静态链接；不复制 DLL，不引入 RustDesk 代码。
4. 本机状态、PID、身份密钥、证书、日志、构建产物和临时验收程序不提交；测试凭据不进入仓库。
5. 仅在用户明确要求时提交或推送；推送保持 fast-forward，不改写历史。

## 6. 恢复入口

```powershell
git switch codex/development-checkpoint-2026-11-09
git log -1 --oneline
git status --short

# 原生构建与关键回归（依赖前缀见检查点文档）
cmake -S . -B build -DVPL_ROOT=D:/vpl_install_static -DFFMPEG_ROOT=D:/vcpkg/installed/x64-windows-static -DOPENH264_ROOT=D:/vcpkg/installed/x64-windows-static-md -DVPX_ROOT=D:/vcpkg/installed/x64-windows-static-md -DOPUS_ROOT=D:/vcpkg/installed/x64-windows-static -DSPEEXDSP_ROOT=D:/vcpkg/installed/x64-windows-static -DMSQUIC_ROOT=D:/vcpkg/installed/x64-windows
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure

# Go 服务端
cd server; go test ./...

# Flutter 查看器
cd app; flutter analyze; flutter test
```

阅读顺序：本文件 → [开发检查点](development-checkpoint-2026-11-09.md) → [RustDesk 全量对照](rustdesk-full-parity-2026-11-07.md) → 对应模块文档。
