# W1 / S03 角色并发编辑保护与验收

日期：2026-11-17。分支：`codex/s03-role-console-2026-11-17`，基线 `202a228` 加当前未提交工作区。状态：**角色更新/删除版本保护、SQLite/PostgreSQL 回归和双编辑者浏览器验收通过**。W1 的完整发布验收仍未完成。

## 1. 问题与行为

两个管理员同时打开同一角色，原实现会让后一次保存覆盖前一次保存的名称、说明和权限。本次为每个角色持久化独立的 `revision`，更新/删除必须提交读取时的版本。数据库通过带版本条件的单条写语句原子判断，冲突不会改变角色、权限、绑定或写入成功审计。

版本为不透明字符串，不是时间戳或权限凭据；创建、每次成功更新都会生成新值。即使两次更新发生在同一毫秒，或角色删除后以同样 ID 重建，旧版本也不能继续写入。普通列表刷新、读取或服务重开不会改变版本。

最后管理员保护失败时整个事务回滚，权限和版本均保持原值；删除匹配版本的角色后，原有外键级联清除绑定。内置角色继续不可编辑/删除。API 仍先执行身份、权限和角色权限上限校验，版本条件不会代替授权。

## 2. 接口契约与升级兼容

角色创建、详情、列表和更新响应增加 `revision`。创建请求不需要版本；以下写入条件是必填项：

| 操作 | 前置条件 | 成功 |
| --- | --- | --- |
| `PUT /v1/admin-roles/{id}` | JSON 中 `if_revision` 等于读到的 `revision` | 200，返回新 revision |
| `DELETE /v1/admin-roles/{id}` | 查询参数 `if_revision` 等于读到的 `revision` | 204 |

更新示例（示例版本需替换为实际读取值）：

```json
{
  "name": "Device reader",
  "description": "Reviewed permissions",
  "permissions": ["devices.read"],
  "if_revision": "角色读取响应中的 revision"
}
```

对有权管理的自定义角色，缺少版本返回 **428** / `role_revision_required`；版本过期返回 **409** / `role_revision_conflict`；目标已删除返回 **404**。内置角色限制、无权限等错误继续遵守原有状态码。客户端不得自动读取新版本并重发原来的完整权限集合，否则会重新引入覆盖问题。

这是角色更新/删除接口的兼容变更：旧页面和旧自动化调用不能再无条件写入，需刷新管理台或更新调用方传递版本。仓库内调用和测试已更新。绑定创建/解绑 API 的请求格式不变。

数据库升级为 **v26**。SQLite 和 PostgreSQL 为已有角色补齐版本，保持原名称、说明、权限、创建/更新时间和绑定；迁移可重复执行并保留已生成的版本。真正的 v25 schema 升级、重复打开及 v25 程序拒绝写入 v26 数据库均有回归。回滚应恢复升级前备份，不能直接删除迁移记录或用旧程序继续写入。

## 3. 管理台交互

- 编辑器保留打开时的角色版本，定时/手动刷新列表不更换草稿的基准版本。
- 保存冲突时保留名称、说明与权限选择，显示持久提示并禁用再次保存旧版本。
- “重新载入（放弃草稿）”明确替换为服务端最新角色；重新载入失败时继续保留草稿和冲突状态，成功后可编辑重试。
- 角色已删除时保留草稿并提示复制内容或清空编辑器；清空后恢复新建模式。
- 删除携带当前行版本；版本变化时提示未删除并要求刷新核对。
- 继续遵守上一轮忙碌状态、撤权和换号保护，迟到的重新载入结果不能覆盖新账号草稿。

## 4. 自动化证据

实际执行：

```text
node --check server/internal/webadmin/assets/app.js
node --test server/internal/webadmin/app_session_test.cjs
# 28 passed, 0 failed

# server/
go test ./... -count=1 -timeout 10m
# 49 packages passed, 0 failed
go vet ./...
# 通过，无诊断

# 仓库根目录
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test-postgres-console.ps1 -PostgresBin D:\ds-chcast\build\s03-postgres-20261117\runtime\pgsql\bin
# PostgreSQL 14.25: 24 top-level tests passed; 0 failed; 0 skipped; 0 remaining test schemas

git diff --check
```

新增测试覆盖：

- 两个独立 Store 同时更新一个版本，恰好一次成功、一次版本冲突；SQLite 与 PostgreSQL 使用同一测试逻辑。
- 固定时钟下更新、旧版本删除不影响绑定、当前版本删除级联清理、同 ID 重建后拒绝旧版本、最后管理员失败回滚。
- 从真正的 v25 迁移前缀创建旧 schema，升级保留内容和绑定，版本非空且不同，重复打开版本稳定，旧二进制拒绝。
- SQLite/PostgreSQL HTTP 矩阵覆盖 428/409、只读拒绝、有效会话权限仍为先保存者结果、冲突无成功审计、读取新版本后的重试。
- 前端真实函数覆盖冲突草稿保留、轮询不更新编辑版本、显式重新载入、加载失败、换号迟到响应、角色删除恢复、删除版本参数及取消不发请求。

日志：`build/s03-role-revision-20261117/go-full.log`、`go-vet.log`、`node-tests.log`；PostgreSQL 日志与结果为 `build/postgres-console-20261117-114059-25ccd606/`，集群已停止。新增的两个 Store 测试不等于全部多实例授权和最后管理员并发场景已完成验收。

## 5. 真实浏览器证据

使用当前资源构建的手动夹具，SQLite + 内存注册表，仅监听 loopback；两个浏览器标签分别以虚构 manager、root 登录。

1. manager 创建 `concurrent-review`，初始只有 `devices.read`。
2. 两个标签均进入该角色编辑；manager 先保存名称并增加 `audit.read`。
3. root 修改名称、说明并保存旧草稿，页面显示冲突；名称、说明和原先 `devices.read` 选择仍在，保存禁用，重新载入按钮可见。
4. root 手动刷新列表，草稿和冲突保持；显式重新载入后获得 manager 的名称及两项权限，再编辑保存成功。
5. root 再次编辑期间通过本地测试 API 删除该临时角色；页面保存显示角色已删除，保留草稿，点击清空后新建按钮恢复可用。该步不计作原生删除确认框验收。

截图：`build/s03-role-revision-20261117/conflict-preserved.jpg`，展示保留的权限选择、冲突提示、重新载入和禁用保存按钮；名称、说明完整值另由 DOM 确认。

夹具日志：`build/s03-role-revision-20261117/browser1.log`。测试标签已关闭，API/页面端口 61485/61492 已无监听；夹具正常停止的 PASS 仅表示清理成功。

## 6. 下一步边界

下一项为角色删除、解绑的原生确认与取消浏览器验收，以及完整响应式、键盘和焦点检查。本轮双标签未触发原生确认框，上一轮的浏览器控制超时问题尚无解除证据。S01/S02 双 Windows 实机、原生重启/断网和生产部署门槛继续保留，不提前进入 C01/P2。
