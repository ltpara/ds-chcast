# M11 P2-20：ACME 自动证书（chast 自有设计）

状态：完成（2026-10-28）。
目标：`chast-server` 通过 ACME（RFC 8555）自动申请与续期 HTTP API 的 TLS
证书，替代手工 certbot + 文件热加载；不改会话协议（保持 v27），不引入新
的服务端口语义。

## 1. 与 RustDesk 的差异与取舍

hbbs 没有内建 ACME：官方部署依赖反向代理（Caddy/Nginx）或手工证书。
chast 已有证书文件热加载（P2-14），本项补齐“单二进制直接面对公网”的部署
形态。

- 只做 HTTP-01（默认 `:80`），复用现有 HTTP API 端口做 HTTPS；DNS-01、
  泛域名与 TLS-ALPN-01 不做。
- 证书与账户密钥缓存在本地目录（`autocert.DirCache`），续期由 autocert
  在到期前自动完成；缓存丢失只会触发一次重新签发。
- 与 `--tls-cert`/`--tls-key` 互斥（同一监听不能有两个证书来源）；与
  `--tls-client-ca` 兼容，可同时要求客户端证书。
- 只作用于 HTTP API 监听；中继节点证书仍沿用现有旗标，不做 ACME。
- CA 默认 Let's Encrypt 生产目录，`--acme-directory-url` 可切 staging 或
  自建 ACME（Pebble）；支持需要 EAB 的 CA（ZeroSSL/Google 等）。

## 2. 旗标

| 旗标 | 默认 | 说明 |
| --- | --- | --- |
| `--acme-domain` | 空 | 逗号分隔的 DNS 名称；为空关闭 ACME |
| `--acme-email` | 空 | 注册到 CA 的联系邮箱（推荐） |
| `--acme-cache` | 空 | 账户/证书缓存目录；空时取 `--state` 同目录的 `acme-cache`，无状态文件时为 `./acme-cache` |
| `--acme-http-listen` | `:80` | HTTP-01 挑战与 HTTPS 跳转监听；CA 必须能访问 |
| `--acme-directory-url` | 空 | ACME 目录 URL；空为 Let's Encrypt 生产 |
| `--acme-eab-kid` | 空 | 外部账户绑定 Key ID（需要 EAB 的 CA） |
| `--acme-eab-key` | 空 | EAB HMAC 密钥（base64url，CA 下发原文） |

校验：域名为裸主机名（不含端口/通配符/IP，至少一个点）；`--acme-domain`
为空时其它 `--acme-*` 必须保持默认；启用 ACME 时禁止 `--tls-cert`/
`--tls-key`；`--acme-http-listen` 不能为空，且不能与 `--http-listen` 的
端口相同（同一 TCP 监听会冲突）。

## 3. 行为

- 启动时创建缓存目录并绑定 HTTP-01 监听；绑定失败直接退出（部署期暴露）。
- HTTP 监听对 `/.well-known/acme-challenge/` 之外的 GET/HEAD 一律 302 到
  `https://<host><path>`；挑战路径只接受白名单主机名，未知 token 返回 404。
- HTTPS API 的 `GetCertificate` 由 autocert 提供：缓存命中直接返回，未命中
  才走签发（首次握手可能等待数秒）；续期对该监听透明。
- 启动日志与 `/v1/server` 的 `tls` 字段保持不变，新增启动日志
  `acme`（域名列表）与 `acme_http`（挑战监听地址）。

## 4. 验证计划

- 单测：域名解析（去空格/去重/大小写/尾点/通配符/IP/无点拒绝）、EAB 密钥
  base64url 解码、缓存证书命中与白名单外主机拒绝、HTTP-01 监听的 302/404/
  403 行为。
- 回环（真实进程）：预置 `acme.test` 自签证书到缓存目录，启动
  `chast-server --acme-domain acme.test --acme-cache ... --acme-http-listen
  127.0.0.1:18450 --http-listen 127.0.0.1:18443`，`curl --resolve
  acme.test:18443:127.0.0.1 -k https://acme.test:18443/v1/server` 返回
  `"tls":true`；挑战监听对 `/` 返回 302、未知挑战返回 404；同时验证
  `--tls-cert` 与 `--acme-domain` 并用的报错。
- 真实 CA 签发需要公网域名、80 端口与 DNS 解析，保留人工验收（可用
  `--acme-directory-url` 指向 staging 演练）。

## 5. 边界

- 只支持 HTTP-01：无法签发泛域名证书，Cloudflare 代理等遮挡 80 端口的
  部署需要回退到文件证书。
- autocert 账号私钥与证书同目录；目录权限 0700，备份/迁移需一并复制。
- 挑战监听不暴露业务 API，只做挑战应答与跳转；管理台/中继不接入 ACME。

## 6. 验收结果（2026-10-28）

- 单测（`internal/acmecert`）：域名解析归一/去重/尾点，拒绝空串、通配符、
  IP、`localhost`、带端口与下划线；EAB 密钥接受 base64url/base64 四种编码并
  拒绝非法输入；五个无效配置（无域名、无缓存目录、通配符、EAB 只给 KID、
  EAB 只给密钥）全部在构造时报错；预置 `acme.test` 缓存证书后
  `GetCertificate` 返回该证书且
  白名单外主机被拒；`--tls-client-ca` 组合切到 `RequireAndVerifyClientCert`
  且缺文件报错；HTTP-01 监听对 `/` 302 到 `https://acme.test/`、未知 token
  404、白名单外主机挑战 403。
- 回环（真实进程，`build` 临时目录隔离 `state.db` 与缓存）：预置自签
  `acme.test` 到 `--acme-cache`，`chast-server --acme-domain acme.test
  --acme-cache <dir> --acme-http-listen 127.0.0.1:18450 --http-listen
  127.0.0.1:18443` 启动成功；`curl -k --resolve
  acme.test:18443:127.0.0.1 https://acme.test:18443/v1/server` 返回
  `"tls":true`，日志含 `ACME manager configured` 与 `ACME HTTP-01
  listening on=127.0.0.1:18450`，ready 行含 `acme=acme.test` 与
  `acme_http=127.0.0.1:18450`；挑战监听对 `/` 返回 302
  `Location: https://acme.test/`，未知挑战返回 404。
- 负向（启动前退出 1）：`--acme-domain` 与 `--tls-cert`/`--tls-key` 并用、
  只有 `--acme-email` 没有域名、`--acme-http-listen` 与 `--http-listen` 同
  端口、域名 `localhost`、只给一半 EAB 旗标、空 `--acme-http-listen`，错误
  信息与第 2 节校验一一对应。
- 全量：`go vet ./...`、`go test ./... -count=1`、`go build ./...` 通过。
- 未覆盖：真实 CA 签发需要公网域名、DNS 解析与 CA 可达的 80 端口，保留人工
  验收（可先用 `--acme-directory-url` 指向 Let's Encrypt staging 演练）；
  HTTP-01 之外的挑战类型、泛域名与 CA 客户端证书轮换不在本项范围。
