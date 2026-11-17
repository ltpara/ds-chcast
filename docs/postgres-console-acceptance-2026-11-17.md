# W1 / S03 PostgreSQL 验收记录

日期：2026-11-17。工作分支：`codex/s03-role-console-2026-11-17`；代码基线：`202a228`，包括工作区中此前 R1–R5 的权限修复。本轮未提交、推送或切换分支。

结论：本地隔离 PostgreSQL 14.25 实库验收通过，**20 项顶层测试成功、0 失败、0 跳过**，测试结束后残留测试 schema 为 0。覆盖账号存储 10 项、注册表存储 1 项、HTTP 权限 9 项。默认 SQLite 全量测试和 Go 静态检查通过。下一执行项为管理台浏览器 E2E；W1 尚未完成发布验收。

## 1. 环境与隔离

- Windows，PostgreSQL `14.25, compiled by Visual C++ build 1944, 64-bit`，支持 ICU 排序规则。
- 临时运行时来源为 [EDB 官方 Windows 二进制](https://get.enterprisedb.com/postgresql/postgresql-14.25-1-windows-x64-binaries.zip)。运行时、数据库和日志只位于忽略的 `build/` 内，不纳入源码提交。
- `tools/test-postgres-console.ps1` 每次创建独立数据目录和数据库，仅监听 `127.0.0.1` 的空闲端口，使用测试专用 `trust` 认证，结束时停止自身集群并恢复测试环境变量。不会安装系统服务或修改防火墙。该认证配置仅用于临时验收。
- 每个测试通过 `internal/pgtest.Schema` 创建随机 `chast_test_<24位十六进制>` schema，`search_path` 仅包含该 schema。测试关闭 Store 后自动清理 schema，不读写共享 public 表或其他测试的迁移账本。
- 最终运行使用 Windows PowerShell 5.1、端口 `51923`。此前 PowerShell 7 运行也通过。所有本轮临时数据库均已停止，相关端口无监听。

## 2. 覆盖范围

| 验收项 | 实际验证内容 |
| --- | --- |
| 真实旧 schema 升级 | 按原迁移前缀创建 v23，写入旧管理员，升级至 v25；保留旧账号并补齐内置目录 |
| v24→v25 与幂等打开 | 模拟缺少独立应急权限的 v24 目录；升级及再次打开后内置权限正确、自定义角色和绑定不变，账本恰为 25 条 |
| 旧版本边界 | v24 迁移器拒绝 v25 数据库并返回 `TooNewError`；不允许旧程序直接写入新 schema |
| 角色与作用域 | 命名角色继承完整管理能力后可替代旧管理员；最后管理账号的封禁、解绑、删除角色、权限降级被拒绝；全局/组/对象隔离与撤权读取正确 |
| 活动连接 | Store 关闭重开后命令保留；分组/设备过滤先于 limit；相同请求幂等、重复轮询重投、回执重放、精确连接确认关闭且另一 viewer 不受影响 |
| S01 策略 | PostgreSQL 保存、CAS 冲突、设备版本/摘要、回执、移除及旧回执隐藏；备份恢复后状态不变且 CAS 仍有效 |
| 真实备份恢复 | `pg_dump --format=custom --schema=…`，删除本测试 schema，再以 `pg_restore --single-transaction --exit-on-error` 恢复；应用打开前核验迁移账本，随后验证账号登录、大小写规则、角色/绑定、作用域、策略版本/摘要/回执、断开幂等与确认状态；恢复后可解绑撤权 |
| HTTP 权限矩阵 | 9 项 R1–R5 回归使用 PostgreSQL 账号存储：账号提权与维护上限、应急权限独立及范围、删除权限、组/对象集合过滤、机器令牌断开及权限上限 |
| 既有 PostgreSQL 回归 | 账号 CRUD、会话迁移、并发登录锁、设备策略回执、账号组，以及独立的注册表存储测试 |

HTTP 测试中的设备注册表使用内存实现，注册表 PostgreSQL 通过另一独立测试验证；未将整个 HTTP 服务宣称为所有存储均接入 PostgreSQL。备份测试恢复的是一个完整测试 schema，不包含数据库角色、集群配置、外部文件或生产整库灾备。

## 3. 可复跑入口与证据

前置条件：已安装 Go，并准备完整 PostgreSQL Windows 发行包（`bin/` 的同级 `lib/` 和 `share/` 也必须存在），服务端支持 ICU；客户端工具版本须能备份该服务端版本。脚本不会自动下载运行时。

在仓库根目录执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test-postgres-console.ps1 `
  -PostgresBin 'D:\ds-chcast\build\s03-postgres-20261117\runtime\pgsql\bin'
```

脚本实际执行：

```text
go test ./internal/account ./internal/registry ./internal/api -run 'Test(Postgres|PermissionRegression)' -count=1 -timeout 5m -json
20 top-level tests passed; 0 failed; 0 skipped; 0 remaining test schemas
```

最终运行产物目录：`build/postgres-console-20261117-100738-0e127f00/`，包含 `integration.jsonl`、`result.txt`、`version.txt`、`postgres.log`、`pgctl-start/stop.*.log`。该目录内的停止日志确认 `server stopped`。

也可使用预先准备的测试数据库直接运行 Go 测试：设置 `CHAST_TEST_POSTGRES_DSN` 和 `CHAST_TEST_POSTGRES_BIN`。数据库用户须具备测试 schema 创建/删除权限；备份测试要求 DSN 可被 libpq 客户端解析。未设置 DSN 时数据库测试显式跳过，不能据此记为实库通过；未设置工具目录时备份测试跳过。一键脚本将任何跳过视为验收失败。

默认 SQLite 路径另行执行并通过：

```text
# server/，未设置 CHAST_TEST_POSTGRES_DSN / CHAST_TEST_POSTGRES_BIN
go test ./... -count=1 -timeout 10m
go vet ./...

# 仓库根目录
git diff --check
```

全量回归和静态检查日志分别位于 `build/s03-postgres-20261117/sqlite-full.log`、`go-vet.log`。本轮未修改前端、Flutter 或原生实现，未重复使用它们的历史测试结果充作新的实机证据。

## 4. 本轮实现与发现

- 新增 `internal/pgtest` 隔离和备份恢复工具、4 项 PostgreSQL 控制台测试及 PowerShell 验收入口；现有 HTTP 权限矩阵可切换至 PostgreSQL 账号存储。
- 现有 PostgreSQL 测试不再共享迁移账本或依赖前序测试残留账号。v17 升级夹具删除全部后续账本版本；设备策略回执夹具显式创建备用管理员，避免触发“最后管理员不可封禁”。
- 新迁移测试比较迁移前后数据库实际保存的角色，避免用纳秒精度的创建返回值比较毫秒存储值；备份绑定检查比较同一查询接口的恢复前后结果，避免混淆创建响应和详情字段。
- 备份夹具按照真实状态机先轮询投递命令再记录应用回执。Windows PowerShell 5.1 的进程退出码读取通过保留进程句柄修正，最终脚本在该环境实际通过。
- 本轮没有发现需要额外修改生产逻辑的 PostgreSQL 缺陷；上述变更属于测试夹具、验收工具和记录。此前 R1–R5 修复仍保留在当前工作区。

## 5. 继续执行的边界

下一步按既定 W1 顺序完成浏览器中的命名角色创建/修改/删除、内置角色只读、global/group/object 授权与撤销、15 秒权限刷新、退出/换号、失败提示与表单恢复，保留实际页面证据。

随后补齐双 Windows runtime、多 viewer、原生主机与 agent 重启、断网与旧 runtime 隔离。这里的 Store 重开不等于进程崩溃或主机重启；两个 Store 观察撤权不等于完整多实例并发验收。生产配置、整库备份恢复、HA、RPO/RTO 和其他 PostgreSQL 版本仍需独立验证。当前不标记 S03/W1 发布验收完成，也不提前进入 C01/P2。
