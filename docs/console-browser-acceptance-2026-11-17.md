# W1 / S03 管理台浏览器验收记录

日期：2026-11-17。分支：`codex/s03-role-console-2026-11-17`，基线 `202a228` 加当前工作区修复。本轮未提交、推送或切换分支。

状态：**角色创建、编辑、失败恢复、三种绑定及解绑、删除/取消、组/对象账号登录、只读控件、撤权轮询、退出/换号及双编辑者冲突恢复已在真实浏览器验证；全管理台与实机联动验收仍未完成。** run3/run4 证据见第 6 节，v26 并发保护见 [角色并发保护记录](role-concurrency-acceptance-2026-11-17.md)。最新 [删除、解绑与交互验收](role-actions-acceptance-2026-11-17.md) 已完成页面内确认、取消/失败重试和登录、角色页响应式/键盘检查。此前原生确认工具超时为历史记录，不据此认定生产确认框有缺陷。以下保留此前各轮历史记录。

## 1. 验收环境

- 使用当前工作区构建的嵌入式管理台资源和真实 API 路由，SQLite 账号存储、内存注册表，所有服务仅监听 loopback。
- 新增显式启用的 `server/internal/api/console_browser_fixture_test.go`。其 `TestConsoleBrowserFixture` 默认跳过，不是自动化 E2E 测试；设定 `CHAST_BROWSER_FIXTURE_DIR` 后创建虚构 root、manager、reader、east-user、west-user、ordinary 账号，东/西组设备和活动连接。
- `manager` 固定角色为 user，由完整命名角色赋予 18 项全局权限；reader 为只读角色；east-user 为 east 分组角色；west-user 为 westbox 对象角色。
- 测试代理保留真实处理器与资源。验收目录内的 `fail-role-save` 文件使角色写请求返回 503；`delay-role-list` 使角色查询延迟 4 秒；`stop` 文件结束服务并执行清理。延迟换号实验因浏览器阻塞尚未执行。
- 第一次构建的临时目录为 `build/s03-browser-20261117/run1/`；修复后复测为 `run2/`，浏览器 URL 为 `http://127.0.0.1:65433/admin/`。最终说明文案的小幅调整在阻塞后完成，经过语法与回归检查，未计入浏览器复测。

## 2. 实际页面证据

| 场景 | 结果与证据 |
| --- | --- |
| 命名管理员登录 | 通过；固定 user 的 manager 显示“18 项权限”，能进入角色页 |
| 内置角色只读、时间列 | 通过；admin/operator/readonly 行显示只读且无编辑/删除按钮，角色和绑定时间均为正常本地日期时间 |
| 创建、编辑命名角色 | 通过；创建 browser-review，编辑名称并增加 audit.read，列表显示保存后的名称、权限和更新时间 |
| 保存请求失败 | 通过；测试代理返回 503，页面显示失败 toast，名称和三个已选权限保留，保存按钮恢复可用 |
| 失败重试 | 通过；移除故障标记后点击保存，真实 API 和页面列表均更新 |
| 编辑中刷新列表 | 首次发现权限勾选丢失；修复后复测通过，名称与 audit.read/connections.read/devices.read 三项未保存选择保留 |
| 命名管理员查看绑定 | 首次仅有自己 1 条；修复后复测显示 east-user、manager、reader、west-user 共 4 条可管理绑定 |
| global/group/object 创建 | 通过；为 ordinary 分别创建全局、east 分组、westbox/device 对象绑定，列表显示对应范围和正常创建时间；全局范围值输入禁用 |
| 页签选中状态 | 首次切换后 `aria-selected` 仍指向概览；修复后进入角色页立即为 true |
| 解绑与删除确认 | 未完成；点击临时 global 绑定的解绑按钮后，浏览器控制通道超时，`getJsDialog` 未返回弹窗；取消、截图及关闭原标签均失败 |
| 组/对象账号实际登录、撤权轮询、退出/换号、迟到响应 | 未完成；新标签能加载登录页面，但受阻塞影响无法提交，未用 API 回归替代浏览器证据 |
| 多编辑者并发、不同视口完整视觉检查 | 未完成；本轮仅核验了实际显示的角色编辑和绑定区域 |

本地截图：

- `build/s03-browser-20261117/evidence/save-failure.jpg`：失败提示、编辑内容和已选权限仍在，保存按钮可用。
- `build/s03-browser-20261117/evidence/role-bindings.jpg`：角色绑定表单及已创建的绑定。截图为当前视口，三种绑定完整内容另由页面 DOM 读取确认。

## 3. 修复内容

1. **命名管理员绑定列表不完整。** 原先按固定 admin 角色决定是否可查看其他账号绑定，导致有完整命名管理权限的 user 只能看自己。现在允许查看自己的绑定，或在持有全局 `roles.write` 时查看权限集合不超过调用凭据上限的绑定；服务器密钥保持管理能力。只读调用者仍只看自己，受限机器令牌不会借所有者的 admin 身份扩大范围。
2. **角色刷新清空编辑中的权限选择。** 重建权限复选框时保留当前草稿，包括用户主动取消全部权限的情况；不使用后台已保存值覆盖未保存选择。
3. **页签无障碍状态滞后。** `switchTab` 在切换页面时同步 `aria-selected`。
4. **对象绑定控件在只读状态被重新启用。** `syncRoleBindingScope` 同时考虑 `roles.write`，避免角色写权限撤销后重新启用对象类型输入框。该项由代码检查发现并经定向回归验证，尚未完成浏览器撤权复测。

角色编辑提示同时明确：服务端保存后立即按新权限鉴权，页面自动更新；避免误导为必须刷新登录会话才生效。

## 4. 自动化验证

新增 `TestPermissionRegressionNamedManagerBindingVisibility`，覆盖命名管理员跨账号可见性、用户名筛选、强角色不超出权限上限、只读账号只看自己、受限机器令牌、可管理绑定解绑和强绑定拒绝。该用例在 SQLite、真实 PostgreSQL 均通过。

前端新增真实函数回归，验证刷新保留未保存复选框状态，以及 `roles.write` 撤销后对象类型输入框保持禁用。

```text
node --check server/internal/webadmin/assets/app.js
node --test server/internal/webadmin/app_session_test.cjs
# 16 tests, 16 pass, 0 fail

# PostgreSQL 一键脚本，Windows PowerShell 5.1
# 21 top-level tests passed; 0 failed; 0 skipped; 0 remaining test schemas

# server/
go test ./... -count=1 -timeout 10m
go vet ./...

# 仓库根目录
git diff --check
```

上述命令均通过；默认 Go 全量回归为 49 个包成功、无失败。PostgreSQL 最新日志：`build/postgres-console-20261117-105054-112f253d/`，集群已由脚本停止。Go/Node 检查日志：`build/s03-browser-20261117/`。两轮浏览器夹具也已正常停止，相关 API/代理端口均无监听。浏览器夹具结束时打印的 PASS 只表示夹具正常停止，不代表上述未执行场景通过。

## 5. 复跑与下一步

准备一个新的空目录作为 `CHAST_BROWSER_FIXTURE_DIR`，在 `server/` 执行：

```powershell
$env:CHAST_BROWSER_FIXTURE_DIR = 'D:\ds-chcast\build\console-browser-new-run'
go test ./internal/api -run '^TestConsoleBrowserFixture$' -count=1 -timeout 60m -v
```

读取该目录的 `fixture.json` 获得本次 URL 和虚构登录信息，再从真实浏览器操作。结束时在该目录创建 `stop` 文件；测试自动关闭前端代理、API 及存储。默认 Go 回归不会启动此服务。日志和临时数据位于忽略的 build 目录，不提交数据库、凭据或运行产物。

继续前需解除当前内置浏览器确认框阻塞，或准备可由浏览器工具控制、支持确认框的浏览器环境。之后重跑解绑/删除确认，再验证只读账号、组/对象实际登录与列表隔离、撤权后 15 秒内页面收敛、退出/换号和迟到响应。完整并发编辑及双 Windows/原生重启/断网/生产验收继续保留。W1/S03 不标记完成，不提前进入 C01/P2。

## 6. 后续完善：作用域、撤权与异步状态验收

2026-11-17 在同一分支和未提交工作区继续。新启动的 run3（API 55390 / 页面 55392）与 run4（API 59081 / 页面 59083）均为隔离的虚构数据环境；run4 包含本节全部修复。夹具增加 `delay-role-save` 文件开关，使角色写请求延迟 4 秒，便于观察保存中的编辑器状态。两个服务最终正常退出，四个端口均已无监听。

### 6.1 新增修复

1. **概览页权限与 API 不一致。** 原页面凭 `devices.read` 显示概览，但 `/v1/stats` 要求 `ops.read`，只读角色首次登录即出现无权限错误。现改为按 `ops.read` 显示概览；只有设备读取权限时默认进入设备列表。此问题在浏览器发现，修复后重新构建并复测通过。
2. **同一账号撤权后的旧角色查询可能回写。** 权限变化时清空角色和绑定数据、编辑草稿及绑定表单，并使旧请求失效；角色页加入现有 15 秒轮询，按新权限重新加载。旧请求的结束处理不能解除新请求的加载锁。
3. **保存中的控件可能被权限刷新提前启用。** 角色写操作共用独立的忙碌状态，保存期间禁止修改、切换角色、重置编辑器或重复提交；失败后保留草稿，成功后重查列表。撤权或退出换号后，旧写操作不能再清空新编辑器、弹出旧成功提示或解除新写操作的锁。
4. **写入与列表刷新交错导致陈旧显示。** 保存完成会使保存前发出的列表查询失效，确保重新读取已完成的修改，而非因加载标记跳过刷新。
5. **退出后绑定草稿残留。** 清理角色面板时同时重置绑定用户名、范围、对象类型、角色选择项和分组候选项。

这些变化处理单页面请求交错和登录状态；尚未为服务端角色更新增加版本条件，也不能据此宣称多编辑者冲突覆盖已解决。

### 6.2 新增真实浏览器证据

| 场景 | 实际结果 |
| --- | --- |
| east 分组账号登录 | east-user 只显示设备、活动连接、我的账号三个页签；仅有 eastbox 设备和其两条连接，无 westbox |
| westbox 对象账号登录 | west-user 只看到 westbox 设备及其两条连接，无 eastbox；与 east-user 在同一标签页退出后换号 |
| 只读角色页面 | reader 的角色、绑定表单和全部行写操作均禁用；绑定列表只显示 reader 自身 |
| 只读账号默认入口 | 修复前概览显示 API 权限错误；run4 修复后默认进入设备页，概览隐藏，显示正常设备列表 |
| 延迟角色保存 | 4 秒写延迟期间，名称、保存、清空、绑定输入和行编辑按钮均禁用；返回后 race-check 出现在列表，编辑器清空并恢复可用 |
| 退出清理绑定表单 | manager 填入 ordinary / object / eastbox / device 草稿后退出，reader 登录后用户名、范围值、对象类型为空，范围回到 global |
| 角色读取权限撤销 | 用测试 API 撤销 reader 的虚构绑定，浏览器轮询后只剩“我的账号”；角色和绑定 DOM 行数均为 0 |
| 作用域撤销 | 用测试 API 撤销 east-user 的虚构分组绑定，浏览器轮询后只剩“我的账号”；设备与连接 DOM 行数均为 0 |
| 保留读取、撤销管理写权限 | manager 编辑草稿期间将其角色降为 3 项读取权限；轮询后草稿清空、所有写控件禁用，绑定列表从跨账号集合收敛为自己的 1 条 |
| 查询延迟下退出换号 | manager 的角色列表请求延迟 4 秒，期间退出并登录 reader；最终仅显示 reader 自己的 1 条绑定、3 项权限，写入禁用，未出现旧账号成功或错误提示 |
| 删除确认 | 点击临时 race-check 的删除按钮再次触发浏览器输入通道超时，`getJsDialog()` 无结果；未继续接受确认，删除未计为通过 |

轮询场景核验了自动收敛后的最终状态，刷新间隔配置为 15 秒；本轮未采集精确时延分布，不作严格 15 秒 SLA 承诺。延迟换号时首次 DOM 等待超时，随后读取已稳定页面确认了上述结果。撤权数据通过本地测试 API 设置，不计为浏览器解绑成功。

新增截图位于 `build/s03-browser-20261117/evidence/`：`east-scope.jpg` 展示 east-user 的两条 eastbox 连接；`reader-roles.jpg` 展示只读角色页面；`scope-revoked.jpg` 展示撤权后的账号页；`manager-revoked.jpg` 展示降权后的禁用控件。后者输入框内的灰字为 placeholder，DOM 确认字段值为空。完整行过滤和控件禁用数量另经 DOM 验证。

### 6.3 本轮检查与剩余事项

```text
node --check server/internal/webadmin/assets/app.js
node --test server/internal/webadmin/app_session_test.cjs
# 24 passed, 0 failed

# server/
go test ./internal/api ./internal/webadmin -count=1 -timeout 10m
# 两个包通过

git diff --check
```

新增 8 项 Node 回归覆盖概览权限映射、撤权后迟到列表、换号后的加载锁、保存中重复提交、撤权后的写入回调、旧写入影响新账号、保存后的陈旧查询，以及退出绑定草稿清理。执行真实业务函数，只模拟 DOM 和网络。

日志：`build/s03-browser-20261117/run4-node-tests.log`、`run4-go-tests.log`、`run3.log`、`run4.log`。本轮没有改生产数据库或授权 API，因此沿用第 4 节的 21 项 PostgreSQL、49 包全量 Go 和 vet 历史证据，未将其描述为本轮重跑。夹具 PASS 仅表示测试服务正常清理。

剩余门槛：可操作原生确认框的浏览器环境中的删除/解绑及取消流程、多编辑者冲突处理、完整响应式视觉检查、双 Windows 实机、原生重启/断网和生产部署。W1/S03 保持待完整验收，未切换分支、提交或推送。
