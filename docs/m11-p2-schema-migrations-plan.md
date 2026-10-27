# M11 P2-21：版本化 schema 迁移（chast 自有设计）

状态：完成（2026-10-28）。
目标：给 `chast-server` 的 SQLite 状态库引入前向、带版本记录的迁移，
旧库升级可追踪、重复启动不重跑回填，并阻止旧二进制打开更新版本的库。
不改会话协议（保持 v27），不新增旗标或接口。

## 1. 现状与动机

`internal/account` 与 `internal/registry` 共用 `--state` 指向的同一个
SQLite 文件，但各自持有连接。此前两个存储在每次 `Open`/`NewStore` 时都
无条件执行 `CREATE TABLE IF NOT EXISTS`、`PRAGMA table_info` 加列与数据
回填（会话 ID、地址簿修订、管理员角色等）。这带来三个问题：

- 每次启动都重复扫描与回填，库越大启动越慢，且无法判断“迁移是否已做过”。
- 迁移只存在于代码顺序中，后续新增步骤无法依赖明确的前后关系。
- 新版本二进制写过的新表/新列，旧二进制会照常打开并继续写，缺少降级保护。

RustDesk 的 hbbs 没有对应的公开迁移机制（其 SQLite 表结构由 Rust 代码
内联创建），因此这里是 chast 的运维设计，不涉及协议对齐。

## 2. 设计

新增 `internal/schemamig`：

- `Migration{Version, Name, Apply}` 为单向前进步骤，版本号必须从 1 连续。
- `Run(db, table, migrations)` 负责：创建记录表（`version`/`name`/
  `applied_at`）、读取当前版本、拒绝高于最后一个步骤的版本
  （`TooNewError`）、按序执行缺失步骤并逐条记录。
- 步骤自身提交，记录随后写入；进程在两者之间退出时，下次启动重跑该步骤，
  因此步骤必须幂等（基线天然幂等）。
- 记录表名按存储区分：账号库 `schema_migrations`，对等库
  `registry_schema_migrations`。两者共享文件但不共享版本号。

两个存储的基线 `v1` 就是原有逻辑的搬运，行为不变：

- 账号：全部建表/索引 → `migrateGrantScopes` → 25 个加列 →
  `backfillSessionIDs` → `backfillAdminRole` → 三个唯一索引 →
  `backfillRevisions`。
- 对等库：`peers` 建表 → `ensurePeerColumns`（alias/tags）。

已存在的库（包括旧版本二进制创建的库）没有记录表，`MAX(version)=0`，启动时
把基线完整跑一遍（幂等）并写入 v1；之后的启动直接跳过。

## 3. 行为

- 全新库：首次启动执行基线并记录 `version=1 name=baseline`。
- 旧库升级：无记录表时补跑基线并补记 v1；已加过的列按“已存在”跳过。
- 重复启动：不重跑基线，地址簿修订、会话 ID 等回填不会再次修改数据。
- 降级保护：库中版本高于二进制支持时，`chast-server` 在启动阶段退出 1，
  错误为 `schema version N is newer than this binary supports (M);
  upgrade the server binary`，不会带着旧代码继续写入。
- 多实例冷启动：v1 的建表/加列/回填均幂等，版本记录用
  `INSERT OR IGNORE`，两个实例同时首次打开仍然安全（现有并发用例覆盖）。

## 4. 验证（2026-10-28）

- `internal/schemamig` 单测：全新库执行全部步骤、重复运行不重跑、只执行
  缺失步骤、未来版本返回 `TooNewError` 且不执行任何步骤、空列表/跳号/
  缺 Apply/非法表名四类配置错误。
- `internal/account`：基线记录为 v1；人为把 `book_revision` 置 0 后重开，
  值保持不变（证明未重跑回填）；写入 v2 后 `Open` 拒绝。
- `internal/registry`：`registry_schema_migrations` 记录 v1 且重开只有一行；
  写入 v2 后 `NewStore` 拒绝。
- 既有旧库用例（授权重建、修订回填、角色提升、会话回填、pre-label peers、
  并发打开、JSON 状态导入）全部继续通过。
- 真实进程回环：全新 `--state state.db` 启动后两张版本表均为
  `v1 baseline`；重启正常 ready；删除两张版本表后重启自动补记 v1
  （模拟旧库升级）；分别向两张表写入 v2 后启动均退出 1 并打印上述错误。
- `go vet ./...`、`go test ./... -count=1`、`go build ./...` 通过。

## 5. 边界

- 只做前向迁移，不提供 `down`/回滚；降级需要从备份恢复。
- 只在 SQLite 上实现；Postgres 后端仍未实现（开发机无 PostgreSQL/Docker，
  无法做真实库验收），本项为路线图第 28 项中可离线验证的一半。
- 升级前应停止所有旧实例；不支持新旧二进制同时写同一个库的滚动升级。
- 基线在已记录 v1 后不再执行，因此跨版本混跑时旧实例写入的“缺失新列”
  数据不会被新实例自动补回填。
