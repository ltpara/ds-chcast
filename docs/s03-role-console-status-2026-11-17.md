# S03 角色管理台完成状态

日期：2026-11-17

状态：**管理台代码闭环完成，外部验收未完成**。本记录只确认本工作区的代码、自动化和本地 HTTP 资源验收结果，不把待验收项写成已通过。

## 1. 已完成范围

- 管理台保存登录、刷新及 `/v1/account` 返回的稳定权限键，并据此控制页签、按钮、编辑器和只读状态；
- “角色”页面支持命名角色创建、编辑、删除、权限多选，以及全局、分组、对象作用域绑定和解绑；
- 内置角色在管理台保持只读；异步数据加载完成后重新应用写权限，global 范围值不可编辑；
- 退出登录清除旧账号的角色、绑定、编辑器、权限选项和在途响应状态，换号后旧响应不能覆盖新账号；
- 修复角色更新响应丢失 `created_at` 的问题，事务内保留原创建时间；
- 修复管理台时间列对毫秒数、RFC3339 字符串和无效值的显示处理。

## 2. 关键文件

- `server/internal/account/admin_roles.go`
- `server/internal/account/admin_roles_test.go`
- `server/internal/api/admin_roles_api_test.go`
- `server/internal/webadmin/assets/app.js`
- `server/internal/webadmin/assets/index.html`
- `server/internal/webadmin/app_session_test.cjs`
- `server/internal/webadmin/webadmin_test.go`

## 3. 本轮验证证据

实际执行并通过：

```text
node --check server/internal/webadmin/assets/app.js
node --test server/internal/webadmin/app_session_test.cjs
  tests 9, pass 9, fail 0
go test ./internal/account ./internal/api ./internal/webadmin -count=1
git diff --check
```

新增回归覆盖：

- 编辑角色后 `created_at` 保持不变，`updated_at` 正常返回；
- 角色 API 的创建时间和更新时间字段可被消费；
- `fmtTime` 正确格式化毫秒数、RFC3339 字符串，非法值返回 `—`；
- 晚到 401 不跨账号重试，退出后的刷新不能恢复凭据，并发刷新合并；
- 换号时旧角色响应被拒绝，退出清空角色状态；
- 管理台角色页面、稳定权限契约和活动连接状态回归。

本地临时服务 `http://127.0.0.1:18443/admin/` 已返回本轮构建的 `app.js`，其中包含 `fmtTime`、角色/绑定时间列和删除角色提示文本。该证据只证明本地资源已更新，不等同于浏览器 E2E 或生产验收。

## 4. 尚未完成的验收

- 使用隔离 PostgreSQL 实例执行 v23 迁移、重复迁移、备份恢复和旧版本升级；
- 使用命名角色账号完成管理台浏览器 E2E：登录、时间列、只读内置角色、临时角色创建/编辑/删除；
- 验证 global/group/object 绑定和解绑、权限撤销即时生效、失败 toast、表单保留与重试；
- 验证退出和换号后旧角色行、编辑器和权限复选框不残留；
- 验证无 `roles.write` 权限的账号看不到或不能使用写操作；
- 完成视觉、并发编辑和完整越权回归。

在上述项目具备环境并通过前，S03 只能标记为“管理台代码闭环、外部验收待完成”，不能标记为“已完成”或“已全面对齐 RustDesk”。

## 5. 工作区边界

本轮测试使用的 `test-temp/`、临时服务二进制、浏览器缓存和探针文件均为本地验收产物，不纳入本次提交。临时账号和绑定也不写入仓库。仅提交明确的 S03 源码、测试和状态文档。