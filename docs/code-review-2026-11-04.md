# chast 项目代码审核报告

- 审核日期：2026-11-04
- 审核范围：仓库全部 C++（`src/`、`include/`、`app/windows/`）、Go 控制面（`server/`）、Flutter 客户端（`app/lib/`）、构建与打包脚本
- 审核方式：只读静态审核。按模块分工通读关键路径，再对高危结论逐条回读源码复核；未修改任何代码
- 环境限制：本机没有 Flutter SDK，且 Go module 代理不可达，因此**没有运行** `go test`/`go vet`、`flutter analyze`、C++ 构建与 ctest。下文结论全部来自源码证据，不包含运行验证

---

## 0. 结论先行

- **默认部署不安全**：`chast-server` 默认 `--server-key` 为空、监听 `0.0.0.0:8443`、账号库默认开启。此时管理 API 对全网无鉴权，任何人可以创建管理员账号、重置密码、注册主机；relay 也退化成开放中继。这是本次审核唯一一条“默认配置即可接管”的问题（C1）。
- **未认证 DoS 面较大**：rendezvous 的 keepalive 挑战可被任意覆盖（H1），QUIC 服务端在认证前会无限期占住唯一会话槽（H2）。两者都不需要任何凭据。
- **远程内存安全无实际缺陷（H4 复核为误报）**：FFmpeg 解码把网络缓冲直接挂到非引用计数的 `AVPacket` 上，但 `avcodec_send_packet` 会先 `av_packet_ref` 复制并补齐 `AV_INPUT_BUFFER_PADDING_SIZE` 字节；已用 guard page 回归测试固化该契约。
- **账号系统有两处设计缺口（H5/H6，已于 2026-11-05 修复，见各节）**：2FA 绑定不要求二次认证（H5），refresh token 宽限让重放检测失效（H6）。
- **媒体接收无界与 QUIC 句柄并发（H3/M1/M2，已于 2026-11-05 修复，见各节）**：四条媒体队列加了字节高水位反压，句柄与 parser 统一到同一把会话锁。
- **授权撤销在“仅文件”会话中不生效**，且旧掩码会带到下一次会话（H7）。

其余问题集中在：QUIC 句柄/解析器的并发纪律、审计与日志的可信度、本地文件接收的 TOCTOU、客户端 HTTP 通道的机密性与首次信任。项目整体工程质量高于同类平均水平，**修复 C1/H1/H2 的投入产出比最高**（H4 已复核为误报，见该节）。

---

## 1. 总体评价

这是一个完成度相当高的远程桌面/串流项目。几个超出同类项目平均水准的地方：

- **线协议**有统一的 `Reader` 边界检查，所有长度/计数/尺寸在分配前都有常量上限，未知枚举值基本 fail-closed；`src/net/protocol.cc` 未发现堆溢出或整数溢出。
- **QUIC 层**把每项能力（输入、剪贴板、文件、端口转发、终端、摄像头、麦克风、隐私模式、声音）都放在接收点做 `session_permissions` 复查，批准掩码还会与 `permission_ceiling`、`config_permissions()` 两次求交，未发现提权路径。证书固定是强制的：没有 64 位 hex pin 就必须显式 `allow_any_certificate`。
- **文件接收**使用 `\\?\` 全路径、逐段拒绝 reparse point、`.part` + sidecar（size+SHA-256）先写后验、`MOVEFILE_WRITE_THROUGH` 改名、16 GiB 上限，路径穿越未发现可利用点。
- **Go 账号系统**全部使用 `Authorization: Bearer`（无 cookie，因而不存在 CSRF/HttpOnly/SameSite 面），token 32 字节随机 + SHA-256 存储，登出/改密/封禁/重置在同一事务内吊销，SQL 全参数化，WebAuthn 的 challenge 一次性消费、origin/RP ID/签名计数器校验完整。
- **更新包**签名覆盖 version+URL+SHA256+size，`downloadName()` 限制为单个 `[A-Za-z0-9._-]` 路径元素，先写临时文件、校验摘要、再原子改名。
- Flutter 侧没有 `badCertificateCallback` 放行，所有 `Process.start` 都是参数列表 + `runInShell: false`，未发现命令注入；远端文件名不参与本地路径拼接。

真正的问题不在“解析器写错”，而在**默认配置的安全姿态、未认证控制报文的策略、会话生命周期的一致性、以及少量跨信任边界的内存安全问题**。最严重的一条只需默认启动服务端即可利用。

### 风险分布

| 级别 | 数量 | 说明 |
| --- | ---: | --- |
| Critical | 1 | 默认部署下未认证即可接管管理面/中继 |
| High | 7 | 远程 DoS、远程越界读、账号接管、授权撤销失效 |
| Medium | 17 | 内存安全竞态、审计/日志可信度、本地提权面、客户端密钥暴露 |
| Low | 12 | 健壮性、一致性、清理项 |

> 说明：本报告按“可利用性 + 影响面”定级。需要本机低权限用户配合的（TOCTOU、命令行读取密钥）定为 Medium；需要已认证 viewer 配合但能打挂宿主进程的定为 High。

---

## 2. 严重问题

### C1. 默认空 `--server-key` 让管理 API 与 relay 对全网开放

**位置**
- [`server/internal/api/api.go:1422`](../server/internal/api/api.go#L1422)
- [`server/internal/api/console.go:35`](../server/internal/api/console.go#L35)
- [`server/cmd/chast-server/main.go:62`](../server/cmd/chast-server/main.go#L62)、[`:78`](../server/cmd/chast-server/main.go#L78)、[`:118`](../server/cmd/chast-server/main.go#L118)
- [`server/internal/relay/relay.go:1205`](../server/internal/relay/relay.go#L1205)
- [`server/internal/api/api.go:706`](../server/internal/api/api.go#L706)

**证据**

```go
// api.go:1422
func (s *server) authorized(r *http.Request) bool {
    if s.cfg.ServerKey == "" {
        return true
    }
    ...
}

// console.go:35
func (s *server) consoleActor(r *http.Request, need consolePermission) (account.User, string, bool) {
    if s.authorized(r) {
        return account.User{}, "server-key", true   // 直接是 admin 级
    }
```

默认值全部指向“对外、无鉴权、无 TLS”：

```go
// main.go:62 / 78 / 118（默认值）
httpListen = flag.String("http-listen", "0.0.0.0:8443", ...)
serverKey  = flag.String("server-key", "", "required for registration unless empty")
stateFile  = flag.String("state", "chast-server-state.db", ...)   // 账号库默认开启
```

**影响**

按默认参数启动后，任意能访问 8443 的主机可以：

1. `POST /v1/accounts` 创建账号。空库时 `createUser()` 会把第一个账号自动提为 `RoleAdmin`（[`store.go:589-595`](../server/internal/account/store.go#L589-L595)），攻击者由此获得管理员账号；
2. `POST /v1/accounts/{name}/password` 重置任意账号密码；
3. `GET /v1/accounts` 读取全部用户名、邮箱、手机号；
4. `POST/DELETE /v1/hosts` 注册、改写、删除主机绑定，抢占 relay 端口；
5. 通过 `GET /v1/hosts/{id}` 拿到 `carrier_token`、relay 端点与主机内网地址（[`api.go:706`](../server/internal/api/api.go#L706)）。

同一空 key 还会把 relay 的 viewer 鉴权关掉：`WithViewerTokenRequired(*serverKey != "")`（[`main.go:582`](../server/cmd/chast-server/main.go#L582)）。于是任何人向 41000–41099 任一端口发一个 UDP 包（或 TCP `CHASTT1\n`）就能成为该主机唯一的 viewer，接收主机全部上行数据、占满会话配额、把合法 viewer 挤成 busy（[`relay.go:1018-1049`](../server/internal/relay/relay.go#L1018-L1049)）。

**修复**

- `--server-key` 为空时**拒绝启动**，或自动生成一个随机 key 并在启动日志打印一次；
- 确实需要“开放 relay”时，用独立的显式开关（如 `--insecure-no-server-key`），并强制 `--http-listen 127.0.0.1:8443`；
- relay/carrier 的 viewer 鉴权不要绑定 `--server-key`：每个 lease 始终要求 per-lease token，token 只通过已认证 lookup 下发；
- 默认监听改为回环，公网部署必须显式指定地址。

---

### H1. 未认证的 keepalive/relay_hello 可覆盖唯一挑战槽，一个包流即可让任意主机离线

**位置**
- [`server/internal/rendezvous/rendezvous.go:173`](../server/internal/rendezvous/rendezvous.go#L173)
- [`server/internal/registry/registry.go:601`](../server/internal/registry/registry.go#L601)、[`:629`](../server/internal/registry/registry.go#L629)
- [`server/internal/relay/relay.go:1068`](../server/internal/relay/relay.go#L1068)

**证据**

```go
// rendezvous.go:179：只要求 host_id 已注册，无任何凭据、无限流
nonce, err := s.reg.IssueChallengeScoped(msg.HostID, identity.DomainKeepalive, scope)
...
// registry.go:613-619：每个 (id,purpose,scope) 只有一个槽位，直接覆盖
key := challengeKey(id, purpose, scope)
r.challenges[key] = challenge{nonce: nonce, purpose: purpose, expires: now.Add(challengeTTL)}
```

合法主机随后提交的 `keepalive_auth` 里的 nonce 已经过期，`TouchChallenge` 失败，`LastSeen` 不再刷新；30 秒后 `Online()` 变假，60 秒后租约过期、relay 端口被释放。

`handleKeepalive` 没有使用 `s.limiter`（限流只在 `handlePunchRequest` 中），所以 50 pps 就足够。响应约 200 字节、请求约 50 字节，还可被伪造源地址当作 UDP 反射器；[`rendezvous.go:181`](../server/internal/rendezvous/rendezvous.go#L181) 用不同错误串区分“host 不存在/存在但离线”，本身就是探活 oracle。

`relay_hello` 分支（[`relay.go:1077`](../server/internal/relay/relay.go#L1077)）同样可覆盖主机唯一的 relay nonce，让合法主机的 `relay_auth` 永远失败。

**修复**

- 改成无状态挑战：`nonce = HMAC(serverKey, hostID || candidate || timestamp)`，服务端不需要存 pending；
- 或每个 purpose 保留多个 nonce，只在 nonce 匹配时删除；
- keepalive/relay_hello/carrier 握手全部加 per-IP 限流；
- 两类拒绝统一错误文案，避免主机/端口枚举。

---

### H2. 认证前的连接可以无限期占住唯一会话槽

**位置**
- [`src/net/quic.cc:926`](../src/net/quic.cc#L926)
- [`src/net/quic.cc:4280`](../src/net/quic.cc#L4280)
- [`src/host/host_server.cc:858`](../src/host/host_server.cc#L858)

**证据**

服务端只服务一个 viewer，第二条连接在 listener 回调里直接关闭：

```cpp
// quic.cc:926
HQUIC expected = nullptr;
if (!connection.compare_exchange_strong(expected, event.NEW_CONNECTION.Connection)) {
    api->ConnectionShutdown(event.NEW_CONNECTION.Connection, ...);
    api->ConnectionClose(event.NEW_CONNECTION.Connection);
    return QUIC_STATUS_SUCCESS;
}
```

同时服务端设置了 5 秒 keepalive，使 30 秒 idle timeout 永不触发：

```cpp
// quic.cc:4280
settings.IdleTimeoutMs = config.idle_timeout_ms;      // 30000
settings.KeepAliveIntervalMs = kKeepAliveMs;          // 5000
```

`wait_for_client()` 的唤醒条件是“收到 Hello / transport 失败 / 对端关闭”，不含“已连接但一直不发 Hello”。而 `HostServer` 只在**第一次**会话给 deadline，之后是无限等待：

```cpp
// host_server.cc:857-861
const bool first = stats.sessions == 0;
const auto deadline = first && config_.first_client_timeout_ms > 0
                          ? Clock::now() + ... : Clock::time_point::max();
```

**影响**

攻击者完成 TLS 握手后保持静默（不发 Hello），连接因 keepalive 永不超时，唯一槽位被永久占用。之后所有合法 viewer 都在 listener 回调里被关闭，宿主不会自愈（只有 `first_client_timeout_ms` 覆盖第一次会话）。这是一个不需要任何凭据的持久 DoS。

**修复**

- 在 `wait_for_client()` 中为“已连接未 Hello”加独立超时（如 10 秒），超时主动 `ConnectionShutdown` 并清空槽位；
- 或对 `connected && !hello` 状态不发送 keepalive，让 idle timeout 生效；
- `host_server` 的等待循环对每次新连接都使用有限 deadline。

---

### H3. 视频/音频/麦克风/摄像头接收队列没有上限，可远程打爆内存

**位置**
- [`src/net/quic.cc:2062`](../src/net/quic.cc#L2062)
- [`src/net/quic.cc:1122`](../src/net/quic.cc#L1122)

**证据**

```cpp
// quic.cc:2062
case QUIC_STREAM_EVENT_RECEIVE: {
    HQUIC pause_stream = nullptr;
    if (stream == video.load()) {
        std::lock_guard<std::mutex> lock(mutex);
        copy_chunks(received, event);      // 无高水位、无 pause
        cv.notify_all();
    } else if (stream == audio.load()) {
        ... copy_chunks(audio_received, event);
    } else if (stream == microphone.load()) {
        ... copy_chunks(microphone_received, event);
    } else if (stream == camera.load()) {
        ... copy_chunks(camera_received, event);
```

对比同文件的 CFIL/CPFW/CTTY 分支：它们都有 `pause_stream` 高水位和 `StreamReceiveSetEnabled(FALSE)`。这四条没有，`copy_chunks()` 无条件把每个 buffer 复制进 `std::deque<std::vector<uint8_t>>`，并立刻归还 flow-control credit。

**影响**

- 宿主侧：拿到麦克风权限的 viewer 可以按链路速率持续发 CMIC，而宿主只有在应用调用 `receive_microphone()` 时才消费；不消费时队列无限增长，最终 OOM。
- 观看端：恶意/被入侵的主机可以持续发视频/音频/摄像头帧，观看端解码跟不上时同样无界增长。

**修复**

- 为四条队列加字节高水位（例如视频 32 MiB、音频/麦克风 4 MiB、摄像头 16 MiB）；
- 超过高水位时设置 `event.RECEIVE.TotalBufferLength = 0` 并 `StreamReceiveSetEnabled(stream, FALSE)`，在消费函数降到低水位后重新启用（照抄 CFIL 的写法）。

**修复（2026-11-05）**

四条媒体接收队列改为 `Impl::MediaQueue`（数据块 + 字节计数 + `paused`），高水位取视频 32 MiB、音频/麦克风 4 MiB、摄像头 16 MiB。`queue_media_locked()` 在超过高水位时把 `event.RECEIVE.TotalBufferLength` 置 0（字节留在 MsQuic，扣住流控额度），并在同一把 `Impl::mutex` 内调用 `StreamReceiveSetEnabled(stream, FALSE)`；`receive_frame()`、`receive_audio()`、`receive_microphone()`、`receive_camera_frame()` 每次排空后在锁内降到高水位一半时恢复，且只在流未结束、连接未关闭时恢复。暂停与恢复都持同一把锁，避免“消费者先恢复、回调随后禁用”把流永久停住的竞态。构建通过；本机 Smart App Control 禁止执行新链接的测试二进制，`quic_*` 未做运行时验证。

---

### H4. FFmpeg 解码直接借用网络缓冲，缺少 `AV_INPUT_BUFFER_PADDING_SIZE`（复核：误报）

**位置**
- [`src/codec/ffmpeg_decoder.cc:227`](../src/codec/ffmpeg_decoder.cc#L227)
- 调用点：[`app/windows/viewer/viewer_engine.cc:6360`](../app/windows/viewer/viewer_engine.cc#L6360)

**证据**

```cpp
av_packet_unref(packet_);
packet_->data = const_cast<uint8_t*>(data);
packet_->size = static_cast<int>(size);
packet_->pts  = static_cast<int64_t>(pts_ms);
```

`data` 是 QUIC 收到的 `payload.data()`，长度恰好是帧负载，没有 64 字节尾部填充，也不是 refcounted packet（`av_packet_unref` 已把 `buf` 置空）。FFmpeg 的 `avcodec_send_packet` 在内部 `buffer_pkt` 为空时会 `av_packet_move_ref` 直接接管这个指针，而不是复制；H.264/HEVC 的位流读取器会按 32/64 位整体读取，越过 `data + size` 最多 64 字节。

**影响**

恶意或被入侵的主机构造一个尾部紧贴页边界的 access unit，即可让观看端在解码时越界读堆内存，通常表现为崩溃（ASLR 下远程代码执行难度高，但仍是稳定的远程 DoS，且属于内存安全缺陷）。同仓库的 `src/audio/aac_codec.cc` 明确用 `av_new_packet` 复制，说明作者知道这个约束，视频路径漏了。

**修复**

```cpp
if (av_new_packet(packet_, static_cast<int>(size)) < 0) { ... }
std::memcpy(packet_->data, data, size);
```

或分配 `size + AV_INPUT_BUFFER_PADDING_SIZE`、清零尾部后 `av_packet_from_data`。

**复核（2026-11-05）：不需要修改解码器。**

上述判断对 `avcodec_send_packet` 的数据所有权理解有误。FFmpeg 8.1 的实现（`libavcodec/decode.c`、`libavcodec/packet.c`）是：

```c
ret = av_packet_ref(avci->buffer_pkt, avpkt);   // avcodec_send_packet 先引用/复制
// av_packet_ref: 非引用计数 packet（buf == NULL）
ret = packet_alloc(&dst->buf, src->size);       // size + AV_INPUT_BUFFER_PADDING_SIZE
memcpy(dst->buf->data, src->data, src->size);   // packet_alloc 已清零 padding 尾部
```

`packet_` 的 `buf` 在 `av_packet_unref` 后为空，属于“非引用计数 packet”，因此 `avcodec_send_packet` 在进入解码器前就已经复制到带 64 字节零填充的内部缓冲，解码器读不到调用方的网络内存。`tests/decoder_padding_test.cc` 用一个真实 access unit（截断于 slice 中部）紧贴 `PAGE_NOACCESS` 页放置并解码，任何越界读都会立刻崩溃；保留该测试以固定“packet 不得借用未填充调用方内存”的契约（例如将来改用 `av_packet_from_data` 或自建 `buf` 时会失败）。

---

### H5. 开启 2FA 不需要密码或现码，被盗 token 可绑定攻击者验证器并永久锁死账号

**位置**
- [`server/internal/api/account.go:520`](../server/internal/api/account.go#L520)、[`:545`](../server/internal/api/account.go#L545)
- [`server/internal/account/store.go:1171`](../server/internal/account/store.go#L1171)

**证据**

```go
// account.go:525
user, ok := s.accountUser(w, r)                 // 只校验 bearer token
secret, err := s.cfg.Accounts.StartTwoFactor(user.ID)
...
// account.go:559
err := s.cfg.Accounts.EnableTwoFactor(user.ID, req.Code)   // 只校验新 secret 的 TOTP
```

对比同文件的 disable（[`:583`](../server/internal/api/account.go#L583)）与恢复码接口，都要求 `password + code`，注释也写明 “a stolen token cannot remove the second factor”。启用路径没有这个约束，而且 `enable` 不吊销其他会话。

**影响**

攻击者窃得 access/refresh token 后：`start` 拿到新 secret → 用自己的验证器算码 → `enable`。从此受害者即使密码正确也无法登录（缺 2FA 码会被拒），改密只能吊销 token，2FA 仍绑在攻击者 secret 上，必须运维执行 `user 2fa <name> off` 才能恢复。这是“用被盗 token 把账号永久锁死”的账号接管路径。

**修复**

- `2fa/start` 与 `2fa/enable` 都要求当前密码（或一次短期 re-auth）；
- `enable` 成功后吊销该账号其他会话并写审计；
- 提供基于恢复码/运维的自助解绑路径。

**修复（2026-11-05）**

`StartTwoFactor`/`EnableTwoFactor` 在 store 层校验当前密码；`enable` 在同一事务内启用 2FA 并删除除调用者外的全部 access/refresh 会话，HTTP 层补充 `password rejected` 审计（成功审计改为 `other sessions revoked`）。恢复码登录与 `user 2fa <名> off` 保持为解绑路径。查看器启用对话框同步要求输入当前密码。API/store/Flutter 测试已覆盖错误密码与旧会话吊销。

---

### H6. `RefreshReuseGrace` 让刷新令牌重放检测失效，被盗 refresh token 可与正常客户端长期共存

**位置**
- [`server/internal/account/store.go:1515`](../server/internal/account/store.go#L1515)

**证据**

```go
if rotated != 0 {
    grace := s.refreshGrace.Milliseconds()
    if unixMillis(now)-rotated > grace {
        ... revokeSessionLocked ...
    }
    err = tx.QueryRow(`SELECT token_hash, created_at FROM refresh_tokens
        WHERE user_id = ? AND session_id = ? AND rotated_at = 0`, ...)
    if replacementCreated != rotated { ... revoke ... }
    retire = replacement
}
```

宽限分支只要求“存在一个 `rotated_at = 0` 的替代者，且它的 `created_at` 等于被重放 token 的 `rotated_at`”。替代者自己被轮换后仍然满足这个等式，于是双方可以交替刷新、互不触发撤销。

**影响**

受害者 t1 把 A 轮换成 B；攻击者在宽限内提交 A，合法拿到新 token 并“退休”B；受害者再提交 B，又合法通过。循环下去攻击者持续持有有效 token（`TokenTTL` 720 小时），审计里没有 replay 记录。这是设计层面的重放检测失效，不是实现小错。

**修复**

- 宽限只允许“替代者未被使用且未被轮换”，并在同一事务里记录 `replayed_at`；
- 同一会话出现第二次并发刷新直接 `revokeSessionLocked`；
- 更稳妥的做法是 token family + 使用即失效，不设宽限，客户端用幂等重试处理网络失败。

**修复（2026-11-05）**

`refresh_tokens` 增加 `replayed_at`（迁移 v5）。宽限分支要求被重放 token 从未被重放、替代者仍 `rotated_at = 0 AND replayed_at = 0`，并在同一事务内写入 `replayed_at`；同一会话只要已有一条重放记录，后续任何宽限重放都会 `revokeSessionLocked`。因此“丢失响应的第一次重试”仍然可用，而攻击者与受害者交替提交新旧 token 会在第二次重放时整会话失效。`TestRefreshReuseGrace` 增加了交替重放回归用例。

---

### H7. “仅文件”会话完全忽略操作员的权限撤销，并把过期掩码带到下一次会话

**位置**
- [`src/host/host_server.cc:738`](../src/host/host_server.cc#L738)
- [`src/host/host_server.cc:4777`](../src/host/host_server.cc#L4777)
- [`src/host/host_server.cc:1396`](../src/host/host_server.cc#L1396)

**证据**

```cpp
// host_server.cc:738
void HostServer::set_permissions(uint16_t permissions) {
    pending_permissions_.store(permissions & kPermissionAll);
    permission_update_pending_.store(true);
```

真正把掩码下发到传输层的唯一消费者是 `stream_session()` 里的 lambda：

```cpp
// host_server.cc:1396
auto apply_pending_permissions = [&]() -> bool {
    if (!permission_update_pending_.exchange(false)) { return true; }
    Error applied = session_->set_permissions(pending_permissions_.load());
```

而文件专用会话的主循环没有它：

```cpp
// host_server.cc:4777
while (!stopping_.load()) {
    poll_files(stats);
    ...
}
```

`file_transfer_allowed()` 依赖的 `session_->permissions()` 因此保持不变，`settings_.allow_file_transfer` 也只是启动时快照。

**影响**

viewer 以 `SessionMode::FileTransfer` 连接后，操作员在托盘关闭“文件传输/剪贴板/输入”不会生效：进行中的上传继续、后续 FileOffer/FileListRequest/FileDownloadRequest 继续被接受。会话结束后 `permission_update_pending_` 仍为 true，下一次桌面会话会把上一会话的旧掩码套到新 viewer 上，形成跨会话的授权错配。

**修复**

- 把 apply 逻辑提成 `HostServer` 成员，`file_session()` 每轮调用；
- `end_file_session()`/`begin_file_session()` 清空 `permission_update_pending_` 与 `pending_permissions_`；
- 权限检查不要依赖启动快照，统一走 `session_->permissions() & settings 当前值`。

---

## 3. 中危问题

### M1. QUIC 流句柄在锁外使用，与回调线程的 `StreamClose` 竞争

**位置**：[`src/net/quic.cc:6241`](../src/net/quic.cc#L6241)、[`:2356`](../src/net/quic.cc#L2356)

```cpp
// quic.cc:6241（receive_file_chunk）
if (impl.buffered_file_bytes <= kFileReceiveLowWaterBytes) {
    for (auto& entry : impl.received_uploads) {
        if (entry.second.receive_paused) {
            resume = entry.second.stream;      // 拷出句柄
            break;
        }
    }
}
...
if (resume) {
    impl.api->StreamReceiveSetEnabled(resume, TRUE);   // 锁外使用
}
```

`SHUTDOWN_COMPLETE` 回调会在持锁时把 `entry.second.stream = nullptr` 并 `StreamClose(stream)`（[`:2356-2362`](../src/net/quic.cc#L2356-L2362)）。`resume` 是在锁内拷贝的，解锁后回调可以关闭句柄，随后这里对已关闭的 HQUIC 调用 API，属于 use-after-close。同类模式还出现在 `PEER_SEND_ABORTED` 的 `bulk_aborts` 与 `close_port_forward()`。

**修复**：句柄单一所有权或引用计数；所有 `StreamReceiveSetEnabled`/`StreamShutdown` 调用都在持锁状态下完成，并确认 `SHUTDOWN_COMPLETE` 尚未发生。

**修复（2026-11-05）**

`receive_file_chunk()` 的恢复、`accept_port_forward_connection()` 的上行恢复、`receive_port_forward_data()`/`take_terminal_output()`/`take_terminal_input()` 的恢复、`stop_microphone()`、`finish_camera()`、相机拒绝与 `CameraResult` 发送失败路径，以及 `set_permissions()` 的各撤销分支，都改为在持有 `Impl::mutex` 时用查表得到的同一句柄调用 `StreamReceiveSetEnabled`/`StreamShutdown`。所有 `aborts`/`bulk_aborts` 的 `StreamShutdown` 循环统一走 `Impl::abort_streams_locked()` 并在锁内执行；`close_port_forward()`/`close_terminal()` 在锁内 abort、锁外只做 `StreamClose`。暂停侧同样收敛：`route_bulk_stream()` 与流分类在锁内禁用接收，避免消费者在“解锁后、禁用前”排空并恢复后又被迟到禁用停住。构建通过；运行时验证受 Smart App Control 限制。

---

### M2. 解析器跨线程竞态（麦克风/摄像头）

**位置**：[`src/net/quic.cc:5748`](../src/net/quic.cc#L5748) vs [`:1232`](../src/net/quic.cc#L1232)、[`:8149`](../src/net/quic.cc#L8149) vs [`:1494`](../src/net/quic.cc#L1494)

应用线程在锁外 `push` 麦克风/摄像头 parser，而 MsQuic 回调线程在持锁时对同一个 parser 调 `reset()`。`std::vector` 的并发 `clear()`/`insert()` 是未定义行为，可能读到已释放缓冲区。

**修复**：parser 纳入同一个 mutex，或用 epoch/代际号让旧数据被丢弃而不是并发 reset。

**修复（2026-11-05）**

`receive_frame()`/`receive_audio()`/`receive_microphone()`/`receive_camera_frame()` 不再在锁外 push；取队列、`parser.push()`、`parser.next()`/`consume()` 全部在 `Impl::mutex` 内完成。`microphone_parser.reset()`（`set_permissions()`、`stop_microphone()`）也随调用点进锁。四个 parser 的所有访问现在由同一把锁串行化，`reset_session_locked()` 的清理不再与消费线程并发。

---

### M3. 认证前的畸形控制帧会终止整个监听

**位置**：[`src/net/quic.cc:2088`](../src/net/quic.cc#L2088)、[`:4447`](../src/net/quic.cc#L4447)

控制流解析失败会调用 `note_transport_failure()`，`wait_for_client()` 随即返回非 `WaitTimeout` 错误，`HostServer` 记录 Failed、销毁 session、退避 2 秒重建。也就是说，任何人在完成 TLS 后发一个格式错误的控制帧（或版本不匹配的旧 viewer 发 Hello），就能把宿主从 Listening 打下来，反复操作可让宿主一直不可用。

**修复**：认证完成前，控制流/Hello 解析失败只作废“这一条连接”，不设置 `transport_failed`；只有认证后的协议破坏才升级为会话致命。

---

### M4. 可恢复的拒绝被当成会话致命错误

**位置**：[`src/net/quic.cc:1261`](../src/net/quic.cc#L1261)、[`:1243`](../src/net/quic.cc#L1243)、[`:1275`](../src/net/quic.cc#L1275)

CFIL 无权限、CCLB 无权限、第 9 个并发上传等“可恢复拒绝”走的是 `Kind::Bad` → `note_transport_failure()`，而 CPFW/CTTY/CCAM 用的是 `Kind::Reject`。后果是：撤销剪贴板权限后，仍在途的 CCLB 流可以把整个桌面会话打掉。

另外 `kMaxPendingClipboardBytes = 96 MiB` 只在流分类时检查一次，8 条 40 MiB 的流可以同时通过，聚合上限形同虚设。

**修复**：权限/配额不足一律走 `Reject` 并回明确错误；聚合字节检查移到每次 push。

---

### M5. 撤销文件传输权限不会中止已经开始的“主机→viewer”下载

**位置**：[`src/net/quic.cc:4666`](../src/net/quic.cc#L4666)、[`:6379`](../src/net/quic.cc#L6379)

`set_permissions()` 的撤销分支只 abort `received_uploads`（上传方向），没有处理 `outgoing_uploads`；`send_file_data`/`finish_file_upload` 也不复查权限。于是操作员关掉文件传输后，正在下载的文件会继续发完。`send_clipboard_blob` 等路径都会复查，属不对称缺口。

**修复**：撤销时同时 abort `outgoing_uploads`，并在每个发送块前复查 `session_permissions`。

---

### M6. 发送队列计数跨会话不复位，下一会话可能永久卡在 `send_cv`

**位置**：[`src/net/quic.cc:3914`](../src/net/quic.cc#L3914)、[`:855`](../src/net/quic.cc#L855)、[`:3770`](../src/net/quic.cc#L3770)

拆除时 `cv.wait_for(1500ms)` 与 `wait_sends(500ms)` 的返回值都被忽略，随后无条件 `StreamClose`；`in_flight_*` 从不复位。连接劣化时 viewer 断开，下一个会话继承非零发送队列，`send_cv.wait` 又没有期限，可能永久阻塞，表现为画面冻结且没有错误。

**修复**：会话拆除时强制复位发送计数；`send_cv.wait` 加超时并返回明确错误；不要忽略 `wait_sends` 的结果。

---

### M7. 剪贴板 `CF_DIB` 只校验头/宽高，不校验像素数据长度

**位置**：[`src/clipboard/clipboard.cc:113`](../src/clipboard/clipboard.cc#L113)、[`src/clipboard/clipboard_win.cc:232`](../src/clipboard/clipboard_win.cc#L232)

```cpp
if (data.size() < header_size) { ... return false; }
if (width <= 0 || height == 0 || width > 32768 || height > 32768 || height < -32768) { ... }
```

从不按 `biBitCount`/`biCompression`/`biSizeImage` 推算应有的像素字节数，也接受 12 字节的 `BITMAPCOREHEADER`。这段数据会被原样 `SetClipboardData(CF_DIB)` 放进本地剪贴板，任何粘贴方按头部声明读取，就会越过 HGLOBAL 末尾。

**影响**：拥有剪贴板权限（不需要输入权限）的 viewer 构造 `40×32768×32768×32bpp` 但只有 40 字节数据，主机用户一粘贴就触发越界读/崩溃或巨额分配。

**修复**：按 `stride = ((width*bitcount+31)/32)*4`、`pixel = stride*abs(height)`、调色板大小校验 `data.size()`；`BI_BITFIELDS/BI_JPEG/BI_PNG` 分别处理；宽高上限压到 8192。

---

### M8. TOTP 在 ±1 步窗口内可重放

**位置**：[`src/security/security.cc:721`](../src/security/security.cc#L721)、调用点 [`src/host/host_security.cc:454`](../src/host/host_security.cc#L454)

```cpp
for (int offset = -span; offset <= span; ++offset) {
    ...
    if (constant_time_equal(digits.data(), text, 6)) { return true; }
}
```

`verify_totp()` 是无状态纯函数，同一个 6 位码在约 90 秒内可以被任意次接受。host 对设备签名有 nonce 去重，对 TOTP 没有“已用步号”记录，违反 RFC 6238 §5.2。

**修复**：`verify_totp` 返回命中的步号；`HostSecurity` 持久化 `last_accepted_step`，接受后拒绝 `step <= last`。

---

### M9. 隐私遮罩“假成功”：分层窗口从未设置分层属性

**位置**：[`src/win/privacy.cc:154`](../src/win/privacy.cc#L154)、[`:123`](../src/win/privacy.cc#L123)

```cpp
window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                             WS_EX_TRANSPARENT | WS_EX_LAYERED, ...);
...
if (!IsWindowVisible(hwnd)) { if (ok) *ok = false; return; }
```

全文件没有 `SetLayeredWindowAttributes`/`UpdateLayeredWindow`。未设置属性的分层窗口不会被绘制（透明度视为 0），但 `WS_VISIBLE` 仍为真，`IsWindowVisible()` 照样返回 TRUE。于是 `enable()` 返回成功、host 回 `PrivacyModeCode::Ok`，而屏幕实际没有被遮挡。

**修复**：创建后立即 `SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)`（或去掉 `WS_EX_LAYERED`）；增加覆盖矩形与桌面归属校验；`OpenDesktopW(L"Default")` 失败要直接判失败。

---

### M10. viewer 不发 `audit_ref` 即可完全抑制主机侧审计

**位置**：[`src/host/host_server.cc:963`](../src/host/host_server.cc#L963)、[`:3818`](../src/host/host_server.cc#L3818)

```cpp
if (!settings_.audit_spool.empty() && !audit_ref.empty()) {   // 空 ref => 整条记录不写
```

协议明确允许空 `audit_ref`，而“签名审计”的写入门槛就是它非空。空 ref 还会被复制给 `file_audit_ref_`，让该会话的每次文件传输审计同样跳过。被审计方可以单方面让审计记录消失，正是 `audit_spool.h` 声称要防的场景。

**修复**：主机侧自行生成会话 ID（时间戳+计数器+随机）作为回退，任何会话都落记录并标记 `viewer_ref_absent`。

---

### M11. 日志注入：viewer 可控字符串带换行写入行式日志

**位置**：[`src/host/host_server.cc:3994`](../src/host/host_server.cc#L3994)、[`:4144`](../src/host/host_server.cc#L4144)、[`src/host/service_log.cc:68`](../src/host/service_log.cc#L68)

```cpp
log("file: receiving " + offered_path + " (" + ...);
...
log(cancel.reason.empty() ? "..." : cancel.reason);
```

文件名经过 `sanitize_wide()` 去控制字符，但 `FileOffer.relative_path` 与 `FileCancel.reason` 只校验 UTF-8，`\r\n` 原样进入 `log()`；`log_line()` 是“时间戳 + message + \n”，一条消息内嵌换行就能伪造完整的时间戳行。

**修复**：`HostServer::log()` 入口统一转义 `0x00-0x1F`、`0x7F`（或 JSON/base64 包装）。

---

### M12. 剪贴板文件下载授权表跨会话残留

**位置**：[`src/host/host_server.cc:1651`](../src/host/host_server.cc#L1651)、[`:4202`](../src/host/host_server.cc#L4202)、[`:3631`](../src/host/host_server.cc#L3631)

`.chast-clipboard/<index>` 下载不走接收目录沙箱，直接取 `clipboard_file_paths_[index]`（主机绝对路径）。该数组只在 `poll_clipboard()` 发布本机剪贴板文件列表时赋值，`begin_file_session()`/`end_file_session()` 只清 `last_file_*`。

于是 viewer A 会话期间操作员复制过的磁盘文件路径会留在数组里；viewer B 在下一个会话只要拿到剪贴板权限，请求 `.chast-clipboard/0` 就能让主机把 A 会话期间复制的文件发给自己，即使剪贴板已清空、本会话从未声明任何剪贴板文件。

**修复**：`begin_file_session()` 内加锁清空 `clipboard_file_entries_`/`clipboard_file_paths_`；索引只在“本会话已向该 viewer 宣布过该列表”时有效（记录会话代号）。

---

### M13. 权限变更后重发的 `StreamInfo` 仍携带旧权限位

**位置**：[`src/host/host_server.cc:1192`](../src/host/host_server.cc#L1192)、[`:1433`](../src/host/host_server.cc#L1433)

`stream_info` 用握手时的 `granted_permissions` 构造，`describe_stream()` 用它填 `input_enabled/clipboard_enabled/...`，线上权限字完全由这些字段推导。`apply_pending_permissions()` 只更新了 `audio.enabled`，随后 `refresh_stream(stream_info)` 把**旧**权限位重发；其余重建点（2329/2637/2835/3495/3579）都用 `session_->permissions()`，只有这里漏了。

viewer 会先后收到 `PermissionUpdate(新掩码)` 与 `StreamInfo(旧掩码)`，依赖后者的客户端会继续按“仍允许”显示开关并继续发送。

**修复**：下发前用 `now_mask` 重新构造 `stream_info`。

---

### M14. 剪贴板文本上限不一致：>256 KiB 文本静默丢失且主机剪贴板已被清空

**位置**：[`src/clipboard/clipboard_win.cc:176`](../src/clipboard/clipboard_win.cc#L176)、[`:259`](../src/clipboard/clipboard_win.cc#L259)；对照 [`src/clipboard/clipboard.cc:26`](../src/clipboard/clipboard.cc#L26)

接收侧按 4 MiB（`kMaxClipboardTextBytes`）校验，`set_text()` 却用 256 KiB 的 `kMaxClipboardBytes` 再校验一次。256 KiB～4 MiB 的文本通过全部协议校验后，在 `write_clipboard()` 里 `set_text` 失败；此时 `EmptyClipboard()` 已执行，`apply_peer_clipboard` 静默 `return`，本地剪贴板被清空且无日志。

**修复**：`set_text()` 使用与流一致的 4 MiB 限额；先分配好全部 HGLOBAL、全部成功后再 `EmptyClipboard()`；失败要计数并记日志。

---

### M15. `.part`/`.part.chast` 命名空间与用户文件名重叠，可删除接收目录已有文件

**位置**：[`src/clipboard/file_transfer.cc:894`](../src/clipboard/file_transfer.cc#L894)、[`:967`](../src/clipboard/file_transfer.cc#L967)

```cpp
impl.final_name = unique_name(final_directory, sanitized, error);   // 只避让最终名
impl.part_path = impl.final_path + L".part";
...
DeleteFileW(long_path(impl.part_path).c_str());      // 无条件删除
```

`unique_name()` 只保证最终名不冲突，`part_path` 可能正好是目录里已有的普通文件（用户自己的 `backup.zip.part`，或上次传输完成的 `x.part`），非续传或 sidecar 不匹配时会被直接删除。攻击者先传 `x.part`、再传 `x` 就能删掉自己先前的文件，也能定向删除主机用户任何以 `.part`/`.part.chast` 结尾的文件。

**修复**：把中间态放进与用户命名空间不相交的位置（如 `<root>\.chast-partial\<digest16>`，文件名用哈希而不是对端名字）；确实要复用同目录时，先读 sidecar 确认归属，不匹配就拒绝而不是删除。

---

### M16. 文件接收的 reparse 点检查与实际打开之间存在 TOCTOU

**位置**：[`src/clipboard/file_transfer.cc:902`](../src/clipboard/file_transfer.cc#L902)、[`:969`](../src/clipboard/file_transfer.cc#L969)

```cpp
if (is_reparse_point(impl.part_path) || is_reparse_point(impl.sidecar_path)) { ... return nullptr; }
...
impl.file = CreateFileW(long_path(impl.part_path).c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ, nullptr, CREATE_ALWAYS, ...);
```

先按路径查属性、后按路径打开，中间没有句柄级复核；`\\?\` 前缀不会阻止 Win32 跟随符号链接，`CREATE_ALWAYS` 会截断链接目标。

**影响**：接收目录（默认 `%USERPROFILE%\Downloads\chast`）可被同用户/低权限进程写入时，在检查与打开之间把 `<name>.part` 换成指向受害者文件的符号链接。若 host 以 SYSTEM/服务模式接收而目录落在用户可写位置，就是“普通用户 → 高完整性进程任意文件写/截断”的本机提权面（需要创建符号链接权限，所以定为 Medium）。

**修复**：用 `FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS` 打开后 `GetFileInformationByHandle` 复核 `FILE_ATTRIBUTE_REPARSE_POINT`；或用随机局部名 + 句柄相对创建；接收目录显式设 DACL。

---

### M17. 更新器“校验后按路径执行”，暂存目录默认 `%TEMP%`

**位置**：[`server/internal/updater/updater.go:86`](../server/internal/updater/updater.go#L86)、[`:107`](../server/internal/updater/updater.go#L107)、[`server/internal/updater/launch_windows.go:13`](../server/internal/updater/launch_windows.go#L13)

```go
if strings.TrimSpace(dir) == "" { dir = os.TempDir() }
final := filepath.Join(dir, name)
if err := os.Rename(tmpName, final); err != nil { ... }
return final, nil
```

摘要只在流式写入时算一次，之后按路径名落盘，稍后 `exec.Command(path)` 启动前不再校验、也没有锁住文件句柄。若 `--auto-update-dir`（或默认 `%TEMP%`）对本机其他用户可写，低权限进程可以抢占/替换同名文件，在 agent（可能是 SYSTEM）权限下执行任意代码。

**修复**：校验与执行之间保持独占文件句柄（`FILE_SHARE_NONE` + `FILE_FLAG_DELETE_ON_CLOSE`），或 `Launch` 前重算 SHA-256；默认暂存目录移到安装目录并收紧 ACL。

---

### M18. 本地管理控制台只校验回环地址，没有任何认证

**位置**：[`server/internal/admin/admin.go:162`](../server/internal/admin/admin.go#L162)、[`:218`](../server/internal/admin/admin.go#L218)

```go
ip, ok := remoteIP(conn.RemoteAddr())
if !ok || !ip.IsLoopback() { return }
reply := c.execute(strings.TrimSpace(line))   // 无口令、无 token
```

任何同机低权限用户/进程连 `127.0.0.1:8446` 就能执行 `user passwd <name> <pw>`、`user role x admin`、`user add`、`ab`、`audit`，等于本地提权到 admin/operator。本项目是 Windows 远程桌面场景，多用户主机很常见。

**修复**：启动时生成一次性 token 打印到日志/stderr 并要求首行携带；或改用 Windows named pipe + ACL 限定。

---

### M19. CSV 导出公式注入（审计/连接/文件传输）

**位置**：[`server/internal/api/export.go:131`](../server/internal/api/export.go#L131)、[`:176`](../server/internal/api/export.go#L176)

`encoding/csv` 不会中和以 `= + - @ \t \r` 开头的单元格。用户名、`peer_id`、`detail`、`name` 都可由攻击者控制（失败登录也会写 `audit_events`），运维用 Excel 打开导出文件即可能触发公式（数据外带/钓鱼）。

**修复**：导出前统一 `csvSafe()`：对危险首字符前置 `'`。

---

### M20. UPnP 信任任意 SSDP 响应，构成 SSRF / 端口映射劫持

**位置**：[`server/internal/upnp/upnp.go:85`](../server/internal/upnp/upnp.go#L85)、[`:99`](../server/internal/upnp/upnp.go#L99)

```go
n, _, err := conn.ReadFromUDP(buf)                 // 不校验应答来源
location := searchLocation(buf[:n])
gateway, err := gatewayFromLocation(ctx, location) // 直接 GET 任意 http(s) URL
```

能应答 `239.255.255.250:1900` 的任意 LAN 设备（或能注入该组播的机器）返回 `LOCATION: http://169.254.169.254/...` 或外部 URL，agent 会 GET（上限 1 MB）并用描述里的 `URLBase/controlURL` 发 SOAP POST → 内网 SSRF、映射写到攻击者指定的“网关”。仅 `--upnp` 开启时存在。

**修复**：记录应答源地址，要求 `LOCATION` host 与应答源 IP 相同且同网段；忽略外部 URLBase；只允许私有地址。

---

### M21. relay 转发进来的控制报文被 host agent 当可信，viewer 可让 host 向任意地址发 UDP

**位置**：[`server/internal/agent/host.go:623`](../server/internal/agent/host.go#L623)、[`:808`](../server/internal/agent/host.go#L808)、[`server/internal/relay/relay.go:962`](../server/internal/relay/relay.go#L962)

relay 只拦截 `relay_hello/relay_auth/relay_viewer` 三种类型，其余报文原样转给 host；host 把 relay 端点视为可信控制源（`relayAllowed(src)`），收到 `punch_peer` 就对 `msg.Candidate` 解析并发送 3–6 个 UDP 包。viewer 因此可以让 host 向任意内网/公网地址发 UDP 探测并做 DNS 查询，同时刷日志；还能伪造 `keepalive_ack`（改 NAT 分类）与 `relay_ack`（提前置 `relayReady`）。

**修复**：relay 端点只接受 `relay_challenge`/`relay_ack`；`punch_peer`/`keepalive_*` 只接受 punch 服务器地址；relay 在 viewer→host 方向丢弃 `CHASTC1\n` 开头的报文。

---

### M22. 对端地址“先到先得”，单个数据报即可劫持回程

**位置**：[`server/internal/agent/host.go:701`](../server/internal/agent/host.go#L701)、[`server/internal/agent/viewer.go:734`](../server/internal/agent/viewer.go#L734)

host agent 对任何非控制数据报无条件把 `h.remote` 换成其源地址；viewer agent 在非 loopback 监听时同样。攻击者向 host 的公网 punch 端口（或 relay 端口）发一个 UDP 包，之后 chast-host 发给 viewer 的 QUIC 数据报全被转到攻击者地址，真实会话中断（QUIC 端到端认证保证不泄明文，所以是稳定的路径劫持/DoS）。

**修复**：只有收到带 nonce 的 punch 证明或 carrier 会话建立后才更新 `remote`；host 侧按 QUIC connection ID 分派，而不是单一 `remote`。

---

### M23. TCP accept 一次瞬时错误就永久停服（监听器泄漏）

**位置**：[`server/internal/relay/relay.go:1155`](../server/internal/relay/relay.go#L1155)、[`server/internal/relay/shared.go:203`](../server/internal/relay/shared.go#L203)

```go
conn, err := tcp.Accept()
if err != nil {
    if !errors.Is(err, net.ErrClosed) { log.Warn(...) }
    return            // 监听器既没 Close 也不会再 Accept
}
```

连接洪泛打满 fd → `Accept` 返回 `EMFILE/ENFILE`（不是 `net.ErrClosed`）→ 该端口 TCP/WS carrier 永久失效（UDP 仍通、`/healthz` 仍 OK），共享 carrier 退出后所有依赖 TCP/WS 回退的主机都不可达，`ln` 永不关闭造成 fd 泄漏。

**修复**：临时错误退避重试，只在 `net.ErrClosed` 时退出，退出时 `ln.Close()`。

---

### M24. 客户端 HTTP 通道允许明文，且共享密钥以明文/命令行形式暴露

**位置**
- [`app/lib/server_settings.dart:59`](../app/lib/server_settings.dart#L59)
- [`app/lib/server_settings.dart:124`](../app/lib/server_settings.dart#L124)、[`:178`](../app/lib/server_settings.dart#L178)
- [`app/lib/tunnel_agent.dart:176`](../app/lib/tunnel_agent.dart#L176)

```dart
if (!text.contains('://')) {
  text = 'http://$text';            // 裸主机名默认 http
}
...
Map<String, Object?> toJson() => {'url': url, 'mode': mode.name, 'key': key, ...};
await file.writeAsString(jsonEncode(settings.toJson()));   // 明文
...
if (key.isNotEmpty) ...<String>['--server-key', key],
```

三件事叠加：

1. 连接服务地址允许 `http://`（UI 的 hint 就是 `http://127.0.0.1:8443`）。账号密码（`POST /v1/account/login` 请求体）、服务密钥、账号 token（`Authorization: Bearer`）全部明文过网。QUIC 显示通道有 Ed25519 pin，但 HTTP API 没有任何证书/SPKI 固定，保护不到这些凭据。
2. 服务 Bearer 密钥（同时是 punch 的 HMAC 密钥）明文写进 `%APPDATA%\chast\server.json`，而账号 token 特意走了 DPAPI，处理不一致。
3. 同一密钥以 `--server-key <secret>` 进入 `chast-tunnel` 子进程命令行，同用户任意进程、EDR、4688 审计都能读到。

**修复**：除 loopback 外强制 `https`（或对 API 做 SPKI pin）；服务密钥与账号 token 一样走 DPAPI；密钥改走 stdin/受 ACL 保护的配置文件；拒绝带 `userInfo` 的 URL。

---

### M25. 直连首次连接无条件 `allowUnverified`，pin 写入失败被吞掉

**位置**：[`app/lib/pages/session_page.dart:619`](../app/lib/pages/session_page.dart#L619)、[`:786`](../app/lib/pages/session_page.dart#L786)、[`app/lib/trust_store.dart:96`](../app/lib/trust_store.dart#L96)

```dart
final String pin = await _trustStore.fingerprintFor(widget.target.trustKey);
...
allowUnverified: pin.isEmpty,          // 首次直连：不校验证书
...
if (_pinnedFingerprint.isEmpty && status.serverFingerprint.isNotEmpty) {
  _pinnedFingerprint = status.serverFingerprint;
  unawaited(_trustStore.trust(...));    // 失败也已“记住”
}
```

首次连接被 MITM → 攻击者证书被永久 pin；`trust.json` 写失败（磁盘满/ACL）时内存里看起来已固定，下次启动又变回未验证，静默重复 TOFU。更糟的是 `load()` 对损坏/不可解析的 `trust.json` 直接返回空 map（[`trust_store.dart:62`](../app/lib/trust_store.dart#L62)），一次截断写入就会丢掉全部 pin，所有主机重新回到“首次信任”。这里与服务端身份确认（[`session_page.dart:554`](../app/lib/pages/session_page.dart#L554) 有确认对话框）不对称：主机证书首用没有任何确认。

**修复**：首次连接也弹一次指纹确认；`trust()` 返回空串时提示“无法保存指纹，本次不固定”，并在 UI 上标注未验证；`_save()` 改为临时文件 + 原子 rename，解析失败时保留旧文件而不是清空。

---

### M26. DPAPI 失败时静默降级为明文 token 落盘

**位置**：[`app/lib/account_store.dart:229`](../app/lib/account_store.dart#L229)

```dart
try {
  final String blob = await protect(payload);
  text = jsonEncode(<String, Object?>{'version': 1, 'protected': blob});
} catch (_) {
  // Keep the legacy shape rather than failing the sign-in.
}
...
await file.writeAsString(text);      // 明文 payload
```

DPAPI/原生桥任一失败（CryptoAPI 策略、漫游配置文件、profile 损坏），`account.json` 里就是明文 access + refresh token，用户没有任何提示，而类注释声称 “never touch the disk in the clear”，测试也断言了相反行为。

**修复**：保护失败即放弃持久化（宁可不记住登录），或至少写可见告警；不要静默降级。

---

### M27. 终端 CSI 解析没有长度/工作量上限

**位置**：[`app/lib/terminal/terminal_model.dart:527`](../app/lib/terminal/terminal_model.dart#L527)、[`:797`](../app/lib/terminal/terminal_model.dart#L797)

```dart
if (rune >= 0x20 && rune <= 0x3F) {
  _csiBody.writeCharCode(rune);        // 无上限（OSC 有 512 上限）
}
...
if (value > 999999) { value = 999999; }
...
for (int i = 0; i < count; i++) { line.insert(...); line.removeLast(); }  // 最多百万次
```

恶意/被控主机输出 `ESC[` 后无限跟数字（参数缓冲永不终结，`_dispatchCsi` 还会按 `;` 分配等长 `params`），或发 `ESC[999999L`（9 字节）→ 百万次 `removeAt/insert`，同步跑在 UI isolate 上，造成内存/CPU DoS、界面冻结。

**修复**：`_csiBody` 加 32 字节上限（超限丢弃该序列），`count` 按 `min(count, rows/columns)` 裁剪，对单次 dispatch 的工作量设预算。

---

### M28. WebAuthn 的 rpId/origin 完全由服务器决定，未与所连服务绑定

**位置**：[`app/lib/security_key.dart:84`](../app/lib/security_key.dart#L84)、[`:115`](../app/lib/security_key.dart#L115)、[`app/lib/server_api.dart:425`](../app/lib/server_api.dart#L425)

```dart
final Map<Object?, Object?> reply = await _invoke('securityKeyMakeCredential', {
  'rpId': rpId,
  'origin': origin ?? 'https://$rpId',     // 由服务器给的值反推 origin
```

恶意或被劫持的连接服务返回任意 `rp_id`/`challenge`，客户端不加校验就交给 Windows Hello，使本机认证器为任意 RP 生成凭据/断言，并伪造 `clientDataJSON.origin`，破坏 WebAuthn 的同源保证。

**修复**：校验 `rp_id` 必须是所配置服务器 URL 的 host（或运维白名单），origin 用实际 `settings.url` 的 scheme+host 计算。

---

### M29. 云端录制下载不校验 SHA-256/大小，内容直接写入本地并被“打开”

**位置**：[`app/lib/server_api.dart:3254`](../app/lib/server_api.dart#L3254)

下载循环只把字节写进 `%USERPROFILE%\Videos\chast-recordings\*.mp4`，`recording.sha256` 从未参与比较，也没有大小上限；UI 随后提供“打开/在资源管理器中显示”。服务端被入侵或被 HTTP 中间人替换响应体时，任意字节会落成 `.mp4` 并被用户打开。

**修复**：边写边算 SHA-256 并与 `recording.sha256` 比对，不符即删文件报错；下载前用 `size` 做上限校验。

---

### M30. 采集/音频链路的四类健壮性缺陷

这些都需要本地驱动/设备异常或销毁时序配合，单独看是 Medium，合并列出：

1. **光标形状几何未校验**（[`src/capture/cursor.cc:288`](../src/capture/cursor.cc#L288)）：唯一校验是 `cursor_buffer_size(actual) > raw_.size()`，而 `cursor_buffer_size` 用 `Pitch * Height`，解码却按 `Width*Height*4` 读。驱动上报 `Pitch < Width*4`、`Pitch == 0` 或几何自相矛盾时，堆越界读（最大数十 MB）。应按类型校验 `Pitch >= Width*4`，并用 `GetFramePointerShape` 的 `data_size` 交叉校验。
2. **摄像头 stride 符号未校验**（[`src/capture/camera.cc:154`](../src/capture/camera.cc#L154)）：`MF_MT_DEFAULT_STRIDE` 是有符号值（负值表示 bottom-up），被当 `uint32_t` 读入后转 `LONG` 变负；长度检查用 32 位 `DWORD` 乘法会溢出，随后 `data + (size_t)pitch*row` 变成天文数字指针。应按 `INT32` 读取并拒绝 `<= 0`，用 `uint64_t` 计算长度。
3. **VPL 解码重试无上限**（[`src/codec/vpl_decoder.cc:611`](../src/codec/vpl_decoder.cc#L611)）：`MFX_ERR_MORE_SURFACE` 直接 `continue`，`MFX_WRN_DEVICE_BUSY` 睡 1 ms 后 `continue`，都没有次数/时间上限（编码器有 `kMaxSyncPolls = 10` 作对照）。GPU 卡死时解码线程 100% CPU 空转、永不上报 DeviceRemoved。应加轮询上限并按 DeviceRemoved 交给上层重建。
4. **WASAPI 播放器析构与 `push()` 竞态**（[`src/audio/wasapi_player.cc:542`](../src/audio/wasapi_player.cc#L542)）：`close()` 调完 `stop()` 后不持 `format_mutex_` 就 `swr_free(&swr_)`，`event_` 也不持锁 `CloseHandle`；而 `push()` 在 `format_mutex_` 下用同一个 `swr_` 转换，`push/set_mute/set_lead_ms` 都会 `if (event_) SetEvent(event_)`。销毁与媒体线程并发 → `SwrContext` use-after-free、向已关闭/复用的 HANDLE 投递事件。应持锁释放并原子交换句柄。

---

## 4. 低危与清理项

| # | 位置 | 问题 | 建议 |
| --- | --- | --- | --- |
| L1 | [`src/net/protocol.cc:885`](../src/net/protocol.cc#L885)、[`:1117`](../src/net/protocol.cc#L1117) | `parse_hello_ack` 读了 `reserved` 却从不校验，结尾也没有 `at_end()`；是全文件唯一同时缺这两项的解析器 | 校验保留字节 + 尾部 |
| L2 | [`src/net/protocol.cc:316`](../src/net/protocol.cc#L316) | `decode_frame_header` 不拒绝未知 flag 位，48 字节头的最后一个保留字节从不读取 | 拒绝 `flags & ~0x3`，校验保留字节 |
| L3 | [`src/net/protocol.cc:2764`](../src/net/protocol.cc#L2764) | `make_file_offer` 把非法/空文件名静默变成空名，产出自己解析器必然拒收的消息，文件传输无响应直到超时 | 名字不合法时 fail-fast 返回 `{}` |
| L4 | [`src/net/protocol.cc:2795`](../src/net/protocol.cc#L2795) | FileOffer/FileAnswer/FileResult/FileCancel 不拒绝 `transfer_id == 0`，与全文件其它 id 类消息不一致 | 解析与构造两侧都拒绝 0 |
| L5 | [`src/net/protocol.cc:529`](../src/net/protocol.cc#L529) | `make_hello` 按字节截断 `device_name`，可能切断 UTF-8；`parse_hello` 却强制 `valid_utf8` | 按 UTF-8 边界截断 |
| L6 | [`src/net/protocol.cc:728`](../src/net/protocol.cc#L728) | `make_access_denied` 按字节截断 reason，`parse_access_denied` 不校验编码 | 用 `truncate_utf8_reason` + `valid_utf8` |
| L7 | [`src/net/protocol.cc:1133`](../src/net/protocol.cc#L1133) | `parse_stats`、`parse_input_event` 不校验尾随字节，16 字节消息可填充到 1 MB 仍被接受 | 追加 `at_end()` |
| L8 | [`src/clipboard/clipboard_win.cc:373`](../src/clipboard/clipboard_win.cc#L373) | `DROPFILES` 未清零，`pt`/`fNC` 是残留堆字节，随 `CF_HDROP` 交给剪贴板 | `*drop = DROPFILES{}` |
| L9 | [`src/win/input.cc:568`](../src/win/input.cc#L568) | 协议允许 Back+Forward 合并的 `buttons`，释放时只发一个 XBUTTON 的 UP，另一个永久卡住且 `release_all()` 补不回来 | X 键按位逐个注入，或拒绝多 X 位合并 |
| L10 | [`src/win/sas.cc:43`](../src/win/sas.cc#L43) | `SendSAS` 是 `void`，无权限/组策略禁用时静默失败，函数一律 `return true`，host 回 `SasCode::Ok` | 返回“已请求未验证”，或在 UI 明确提示 |
| L11 | [`src/host/audit_spool.cc:95`](../src/host/audit_spool.cc#L95) | 审计 spool 无容量上限/轮转，`tunnel=false` 时没有消费者删除，永久增长 | 复用服务日志的 5 MB 轮转 |
| L12 | [`src/codec/vpl_decoder.cc:306`](../src/codec/vpl_decoder.cc#L306) | 每次会话建立/表面池分配都 `fprintf(stderr, ...)`，无法关闭 | 删除或降为 debug 日志 |
| L13 | [`include/chast/security.h:71`](../include/chast/security.h#L71) | PBKDF2 迭代 210k 低于 OWASP 2023 建议的 600k，且没有登录后 rehash 路径 | 提到 600k 或 Argon2id；增加 needs_rehash |
| L14 | [`src/audio/wasapi_common.h:224`](../src/audio/wasapi_common.h#L224) | `narrow()` 把结尾 NUL 写进 `std::string` 的终止符槽位，1 字节越界写 | 按 `size` 分配后 `pop_back()` |
| L15 | [`app/lib/server_api.dart:3385`](../app/lib/server_api.dart#L3385) | 录制文件名清洗未处理 Windows 保留名/trailing dot | 复用主机侧 `sanitize_wide` 规则 |
| L16 | [`.gitignore`](../.gitignore) | 未忽略 PEM、日志、`server_identity_audit.jsonl` 等本地产物 | 补规则，避免误提交密钥/日志 |

---

## 5. 已核实、未发现问题的部分

为避免误报，以下方向做了针对性核查，未发现可利用问题：

- **线协议内存安全**：所有长度前缀先与 `remaining()` 比较，再由 `Reader::string/bytes` 二次校验；`pixels*4`、`count*8`、`extradata+20` 等乘加都有常量上界；20 余个 maker 的“计算长度 vs 实际写入”逐条对照，未发现堆溢出；`valid_utf8` 对超长编码、代理项、`>U+10FFFF`、截断序列、NUL 的处理正确。
- **QUIC 证书固定**：没有 pin 时必须显式 `allow_any_certificate`，默认 false；`PEER_CERTIFICATE_RECEIVED` 先比 pin，哈希失败即拒；`normalize_fingerprint` 严格 64 位 hex，未发现绕过。
- **权限模型**：`session_permissions` 是 atomic，服务端对每条 viewer 控制消息和流分类现场复查；`set_permissions` 先降本地掩码再发通知，并与 `permission_ceiling`、`config_permissions()` 求交，未发现提权。
- **文件路径沙箱**：`resolve_receive_path` 逐级拒绝 `..`、`:`（ADS）、`\`、UNC、junction/reparse；`sanitize_received_directory` 拒绝绝对路径/超深路径；`list_receive_directory` 跳过 reparse 与超长名。
- **Go 会话基础**：所有账号接口走 Bearer，无 cookie；token 32 字节随机 + SHA-256 存储；登出/改密/封禁/重置在同一事务内吊销；SQL 全参数化；WebAuthn challenge 一次性消费、origin/RP ID/签名计数器校验完整；录像/下载路径由 DB 行驱动，无穿越。
- **更新包签名**：`identity.Verify(pub, SoftwareMessage(version,url,sha256,size), signature)` 覆盖全部关键字段，`downloadName()` 限制为单路径元素，流式写临时文件、校验 SHA-256、原子改名。
- **Flutter 进程/路径**：所有 `Process.start/run` 都是参数列表 + `runInShell: false`，`explorer.exe` 调用也是数组形式；没有 `badCertificateCallback` 放行；未发现打印密码/token 的日志。
- **中继线程生命周期**：`port_forward_relay`/`terminal_relay`/`camera_relay` 的线程都在 `finish()/stop_console()/pump()` 内 join 后才析构，连接数/终端数由传输层限额。

---

## 6. 测试与工程化观察

1. **无法在本环境验证构建/测试**：没有 Flutter SDK，Go module 代理不可达，MSVC/oneVPL/FFmpeg 工具链未确认。因此报告不包含“测试通过/失败”的结论。建议 CI 至少覆盖：`go test ./...`、`go vet ./...`、`flutter analyze && flutter test`、CMake + ctest（含 `quic_recording_hevc`、`quic_weak_network`）。
2. **测试覆盖偏“功能正确”而非“恶意输入”**：`tests/` 与 `app/test/` 里有大量协议/状态机/UI 测试，但针对本报告中的 fuzz 面（`parse_hello_ack` 尾随字节、`FileOffer` 空名、`CF_DIB` 长度、`FileCancel` 换行、终端 CSI 长序列）没有对应用例。建议为每个解析器补一个“畸形/超长/尾随”用例表。
3. **构建脚本硬编码本机路径**：`CMakeLists.txt` 默认 `D:/vcpkg`、`D:/vpl_install_static`，换机器必须手改；建议用 `CMakePresets.json` + 环境变量，并在 README 写清依赖版本。
4. **文档与实现有漂移风险**：`docs/roadmap.md` 声称的 ctest 数量、协议版本、ENV/DEV 完成度需要与当前树对齐；本报告发现的 C1/H7 都是“默认值/生命周期”类问题，建议在 `docs/status-*.md` 里增加“安全默认值”一节。
5. **日志与审计**：主机侧日志是纯文本行式（易注入），审计 spool 无轮转；建议统一成 JSONL（`ts`、`event`、`fields`），并对所有对端字符串做转义。
6. **依赖**：Flutter 的 `flutter_gpu_texture_renderer` 走 git 固定 commit（好），但 `pubspec.lock` 是否入库、Go 的 `go.sum` 是否校验需在 CI 里确认；建议开启 Dependabot/Renovate 与 `govulncheck`。

---

## 7. 建议的修复顺序

**第一批（一周内，阻断默认部署风险）**

1. C1：空 `--server-key` 拒绝启动 / 强制回环；relay 始终要求 per-lease token。
2. H1：把 keepalive/relay/carrier 挑战改成无状态 HMAC 或多槽位，并加 per-IP 限流。
3. H2：给“已连接未 Hello”加独立超时。
4. H5：2FA start/enable 要求密码；H6：收紧 refresh 宽限（均已完成，见各节修复说明）。

**第二批（两周内，内存安全与远程 DoS）**

5. H4（复核为误报，无需修改）：`avcodec_send_packet` 会复制非引用计数 packet 并补 padding；`tests/decoder_padding_test.cc` 用 guard page 固定该契约。
6. H3：四条接收队列加高水位/反压；M1/M2 统一句柄与 parser 同步（均已完成，见各节修复说明）。
7. H7：文件会话应用权限撤销并清理跨会话掩码；M3/M4 区分“可恢复拒绝”和“会话致命”。
8. M7/M8/M9：CF_DIB 长度校验、TOTP 步号去重、隐私遮罩真正可见。

**第三批（一个月内，可信度与本地边界）**

9. M10/M11/M12/M13：审计回退 ID、日志转义、剪贴板下载表按会话清理、StreamInfo 用新掩码重建。
10. M14/M15/M16：剪贴板文本限额统一、`.part` 隔离目录、句柄级 reparse 复核。
11. M17/M18/M19：更新器句柄锁定、admin console token、CSV 转义。
12. M24～M29：客户端强制 HTTPS、密钥 DPAPI、首用确认、终端 CSI 上限、录制校验。

**第四批（持续）**

13. 低危表逐项清理；补恶意输入测试；统一日志/审计格式；CI 加 `govulncheck` 与依赖扫描。

---

## 8. 附：本次审核的分工与限制

- 线协议（`protocol.h/.cc`）、QUIC/传输（`quic.cc`/`carrier.cc`）、主机服务端（`host_server.cc`/`host_settings.cc`/`audit_spool.cc`）、安全原语/输入/剪贴板/文件传输、Go API/账号、Go relay/agent/updater、采集/编码/音频、Flutter 客户端、原生 Windows viewer，分别通读后交叉复核。
- 所有 `文件:行号` 均对应当前工作树；若后续重构导致行号漂移，以函数名/代码片段为准。
- 本报告是静态审核，不等于渗透测试；未覆盖运行时配置、部署拓扑、第三方依赖漏洞与供应链攻击。
