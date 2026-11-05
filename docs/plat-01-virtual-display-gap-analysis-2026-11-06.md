# PLAT-01 虚拟显示器功能完整性分析（2026-11-06）

本文对照 [PLAT-01 设计文档](plat-01-virtual-display-2026-11-03.md)、
[实机验收手册](plat-01-acceptance-runbook-2026-11-04.md)、当前源码、单元测试
和真实包预检结果，判断虚拟显示器功能是否“完全”，并给出后续开发清单。

## 1. 结论

按第一版设计范围，**核心链路已经实现**：驱动检测与冲突拒绝、固定哈希包校验、
安装/启用/停用/数量调整/卸载、worker 的 `off/auto/on` 策略、安装器和托盘入口、
安装记录与状态目录加固，均有代码和测试覆盖；真实包预检返回
`status=0`、9 个文件、版本 `2.0.0.1`、`guard_active=true`。

但**还不能称为“功能完全”**：

- 一项设计内核心行为未实现：用 `SetDisplayConfig` 把虚拟屏扩展到桌面右侧，
  以及记录原拓扑并在失败时回滚；
- 三项 UI/合规缺口：Amyuni 许可提示、重启状态处理、状态行缺少提供方和最后错误；
- 若干健壮性与易用性小项；
- A1～A10 实机验收全部未执行，目前只有单元测试和“只校验不安装”的真实包预检。

因此代码完成度（开发视角）与可交付状态（验收视角）要分开判断：前者接近
v1 完成，后者仍被专用测试机和 Amyuni 再分发许可阻塞。

## 2. 已实现的部分

| 能力 | 代码位置 | 状态 |
| --- | --- | --- |
| 平台/驱动检测、状态分类、冲突驱动识别 | `src/host/virtual_display.cc:1270`、`:1234` | 完成，单测覆盖分类与冲突 |
| 固定哈希包校验、逐文件 manifest、拒绝 reparse/不可信 ACL、WHQL catalog 主体校验、文件锁守卫 | `src/host/virtual_display.cc:784`、`:1617`、`:1645`、`:1713` | 完成，真实包预检通过 |
| 安装、启用/停用、数量调整、卸载与设备轮询 | `src/host/virtual_display.cc:970` | 完成，未在开发机安装 |
| 安装记录、所有权（避免误卸第三方驱动）、状态目录/记录 ACL 与原子写入 | `src/host/virtual_display.cc:1991`、`:2003`、`:2088` | 完成，单测覆盖 |
| worker 启动策略 `off/auto/on`、无活动显示检测、失败不阻塞会话 | `host/worker.cc:271`、`:367` | 完成，假 provider 单测覆盖 |
| `--vdisplay install/uninstall/set-count/status` CLI 与退出码 | `src/host/virtual_display.cc:2301` | 完成，单测覆盖解析与 status |
| 安装器 `--virtual-display`、卸载先驱动后文件、失败保留文件 | `server/internal/setup/platform_windows.go:242`、`:260` | 完成，未实机安装 |
| 托盘状态项、安装/卸载入口、提权确认、会话中卸载拦截、失败日志尾行 | `host/main.cc:3066`、`:3549`、`:3593` | 完成 |
| `host.ini` 四个配置键与安全设置页策略/数量/分辨率控件 | `src/host/host_settings.cc:715`、`host/main.cc:2258` | 完成 |

验证证据（2026-11-06）：

- `chast-virtual-display-test.exe` 全部检查通过；
- `chast-virtual-display-test.exe --verify-package build\deps\usbmmidd_v2`
  返回 `status=0`、`detail=driver package verified (9 files)`、
  `version=2.0.0.1`、`guard_active=true`，包 SHA-256 与固定值一致；
- 此前全量 `ctest -C Release` 47/48 通过（641.43 s），唯一失败
  `service_log` 为构建后首次启动的瞬时故障，单独复跑通过。

## 3. 缺口清单

### G1（P0）显示拓扑定位与回滚未实现

设计文档 §4.1、§5.3 和 §10.5 要求：

- 用 `SetDisplayConfig` 把虚拟显示器扩展到现有桌面右侧；
- 不改变主显示器、DPI 和用户现有排列；
- 记录原拓扑，失败时回滚。

现状：`UsbmmiddProvider::select_mode()` 只用 `ChangeDisplaySettingsEx` 修改
第一台 usbmmidd 显示器的分辨率（`src/host/virtual_display.cc:1169`、`:1211`），
全仓库没有 `SetDisplayConfig` 调用，也没有拓扑快照或回滚。

影响：Windows 通常会把新启用的显示器默认向右扩展，但这是系统默认行为，
不是代码保证；多显示器、主屏不在最左侧或 DPI 混合场景下可能不符合预期，
失败后也无法恢复原排列。A2/A7 的无头场景尤其依赖该行为。

建议实现：

1. 启用虚拟屏前用 `QueryDisplayConfig` 保存活动路径和模式，计算主显示器
   包围盒右侧的坐标；
2. 只对新增的 usbmmidd 路径应用位置，保持现有路径的 source/target 和主屏
   `sourceMode` 不变；
3. 用 `SetDisplayConfig` 一次性应用；失败时用保存的快照回滚并记录错误；
4. 纯计算部分（右侧坐标、快照差异）做成可单测的函数，不依赖真实驱动。

参考：主线的 `src/capture/display.cc:23` 已有 `QueryDisplayConfig` 读取拓扑的
代码；归档并行分支 `origin/feat/windows-session-display-20261105` 的
`src/win/display_privacy.cc:113` 有 `SetDisplayConfig` 快照/恢复用法。按既定
分支策略 cherry-pick 并适配，不整体合并该分支。

验收：A2、A7 通过；失败注入时快照恢复且原主屏/DPI/排列不变。

### G2（P1）“需要重启”状态未处理

设计文档 §5.3 要求驱动返回“需要重启”时保留安装目录并提示重启后重试，
不能假装成功。

现状：`deviceinstaller64` 的非零退出码一律按通用失败处理
（`src/host/virtual_display.cc:1037`、`:1076`），没有识别
`ERROR_SUCCESS_REBOOT_REQUIRED`（3010）或等价的“重启后生效”结果；安装器
会保留文件，但只显示通用错误。

建议：把退出码映射抽成纯函数并单测；为 3010 增加专门退出码/状态，托盘和
安装器提示“重启后重试”，并保留包目录与安装记录；不要写成功记录。

### G3（P1）Amyuni 许可提示缺失

设计文档 §4.2 要求安装/卸载确认框显示 Amyuni 许可提示。

现状：托盘确认框只提示“改变显示拓扑、需要管理员权限”
（`host/main.cc:3550`、`:3599`），i18n 中没有 Amyuni 或许可相关文案；
`THIRD-PARTY-NOTICES.md` 只在仓库中，未在 UI 展示。

建议：在确认框和 `chast-setup --virtual-display` 输出中加入许可提示与
`THIRD-PARTY-NOTICES.md` 的查看入口；在再分发条款确认前，保持“用户主动
获取、内部验收、不随发布包分发二进制”的现状。

### G4（P2）状态行缺少提供方和最后错误

设计文档 §4.2 要求状态行显示提供方、活动数量和最后错误。

现状：托盘状态只显示状态和数量（`host/main.cc:866`）；安装记录中的
`last_error` 写入了但没有任何读取/展示路径（`include/chast/virtual_display.h:264`），
只有安装失败弹窗读取 `vdisplay.log` 尾行。

建议：`--vdisplay status` 和托盘状态行读取安装记录，显示
`provider=usbmmidd active=N/M last_error=...`；错误为空时省略。

### G5（P2）分辨率下拉未使用驱动实时模式

设计文档 §4.2 要求从驱动支持列表读取分辨率；worker 的 `modes()` 已经对
`EnumDisplaySettings` 和默认列表求交集（`src/host/virtual_display.cc:983`），
但设置页下拉仍硬编码 `usbmmidd_default_modes()`
（`host/main.cc:2890`）。

建议：驱动已启用时用 `make_virtual_display_provider()->modes()` 填充下拉，
未安装/读取失败时回退默认列表，并保留当前配置值。

### G6（P2）托盘缺少独立的启用/停用/数量入口

设计文档 §4.1 提到关闭自动管理后托盘的启用/停用仍然可用。现状只能：

- 在安全设置页修改策略/数量/分辨率，保存后重启共享；
- 用 `--vdisplay set-count 0..4` 手动调整。

建议：托盘增加“启用/停用虚拟显示器”和“数量”菜单项，走 `runas`
`set-count`；会话进行中灰显并提示先结束会话，与 G1 的“会话启动前调整”
约束保持一致。

### G7（P2）健壮性小项

| 项 | 现状 | 建议 |
| --- | --- | --- |
| 冲突驱动检测是 denylist（`mttvdd`/`virtualdisplaydriver`/`parsecvdd`/`rustdeskidd`） | 未知 IddCx 驱动可能漏检 | 结合 `SPDRP_CLASS`/`DEVPKEY` 识别通用 IddCx 虚拟显示，或至少记录未识别显示设备 |
| 虚拟屏计数未检查设备状态 | `count_usbmmidd_monitors()` 只数 present 的 monitor 节点 | 增加 `CM_Get_DevNode_Status`，只统计 `DN_DRIVER_LOADED` 且无 problem code 的节点 |
| 卸载依赖包目录 | 包目录被删除后 `uninstall` 无法运行 deviceinstaller | 在状态记录中保存可用安装器路径，或提示先恢复包目录；保证失败时保留记录可重试 |
| CLI 卸载不感知会话 | 托盘已拦截，直接 CLI 可尝试卸载 | 按验收手册 G3 的说明实现跨进程会话标记，或在 CLI 中查询 worker 状态 |
| 已有 usbmmidd 时不比对版本/哈希 | `install()` 探测到已存在直接返回成功 | 记录实际驱动版本；与包版本不一致时提示升级/复用，不误写新包版本 |
| 部分启用失败不返回实际数量 | `prepare_virtual_display()` 返回错误 detail，实际数量只在 status 中 | detail 中加入 `active=N`，便于托盘和日志判断“至少有一台可用” |
| 实时模式与默认列表无交集时回退默认列表 | 后续 `select_mode` 可能选到驱动不提供的模式 | 优先返回实时模式的子集；交集为空时只应用驱动实时列表中的安全模式 |

这些不属于 v1 阻塞项，但会影响可诊断性和边界场景的可用性。

## 4. v1 明确不包含（非缺陷）

- VDD/MttVDD provider（`make_virtual_display_provider()` 对 `Vdd` 返回
  `nullptr`）；
- ARM64（usbmmidd 只有 x64 二进制）；
- 超过 4 台虚拟显示器；
- 每台虚拟屏独立分辨率（v1 只对第一台应用首选模式）；
- 查看器侧“增加/删除虚拟屏”协议消息；
- HDR、10-bit、自定义 EDID、分数刷新率；
- 自动下载或静默安装驱动；
- 修改注册表自定义分辨率。

## 5. 实机验收状态

| 项目 | 状态 | 备注 |
| --- | --- | --- |
| A1 有物理显示器的安装/连接/卸载 | 未执行 | 需专用 Win10 x64 |
| A2 无头机安装与重启后连接 | 未执行 | 需真正无活动显示的环境 |
| A3 Win11 24H2/25H2 | 未执行 | 2021 年驱动的兼容性未知 |
| A4 Win11 ARM64 | 未执行 | 只需验证托盘/CLI 拒绝路径 |
| A5 已装其他虚拟显示驱动 | 未执行 | 快照回滚机器 |
| A6 会话中卸载 | 未执行 | 托盘入口已加拦截 |
| A7 重启持久性与 off/auto | 未执行 | 依赖 A2 环境 |
| A8 4 台虚拟屏与第 5 台上限 | 未执行 | 需已安装 usbmmidd |
| A9 不支持分辨率回退 | 未执行 | 需已安装 usbmmidd |
| A10 安全模式卸载 | 未执行 | 需快照 |

本机只完成了：单元测试全部通过、真实包 `--verify-package` 预检通过、
`--vdisplay status` 返回 `state=not_installed provider=usbmmidd active=0 max=4`。
**未安装驱动、未改变显示拓扑。**

阻塞项：

1. 专用测试机（Win10 x64、无头机、Win11 24H2/25H2、ARM64、可快照的回滚机器）；
2. Amyuni 再分发许可与许可全文；
3. G1 完成前，无头场景的拓扑行为只有系统默认保证。

## 6. 后续开发顺序

1. **PLAT-01e（G1）**：拓扑快照 + 右侧扩展 + 回滚，补纯逻辑单测；
2. **PLAT-01f（G2/G3）**：重启状态映射、Amyuni 许可提示；
3. **PLAT-01g（G4–G6）**：状态行/最后错误、实时分辨率列表、托盘启停/数量入口；
4. **PLAT-01h（G7）**：健壮性小项，按上表逐条收敛；
5. 每完成一组先跑 `chast-virtual-display-test`、定向 ctest
   （`virtual_display|host_i18n|host_settings`）和 `git diff --check`；
6. 专用测试机就绪后按验收手册执行 A1～A10，全部通过再把 PLAT-01 标记为完成。

在 G1～G3 和 A1～A10 完成前，对外描述应为“虚拟显示器功能已实现、待实机
验收与拓扑收尾”，不要宣称完全可用。

## 7. 参考

- [PLAT-01 设计文档](plat-01-virtual-display-2026-11-03.md)
- [PLAT-01 实机验收手册](plat-01-acceptance-runbook-2026-11-04.md)
- [状态与已知边界](status-2026-10-25.md)
- [第三方声明与许可](../THIRD-PARTY-NOTICES.md)
- [驱动获取脚本](../tools/fetch-usbmmidd.ps1)
- [RustDesk 缺口任务](rustdesk-gap-tasks-2026-11-01.md)
