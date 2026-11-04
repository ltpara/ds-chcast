# PLAT-01 虚拟显示器实机验收手册（A1–A10）

适用代码：`44ee50e`（含 PLAT-01a/c、01b 脚手架、01d 本地接线、ACL/TOCTOU 加固）。
本手册只描述如何在专用测试机上执行 `docs/plat-01-virtual-display-2026-11-03.md`
§8.2 的 A1–A10，不代表这些项目已经通过。2026-11-04 的状态是：**A1–A10 均未执行**，
固定哈希驱动包、Amyuni 再分发许可与专用测试机仍未就绪。

执行前请先读完整份手册。安装显示驱动会改变系统显示拓扑，禁止在开发机或任何
有人正在使用的机器上执行。

## 1. 前置条件

### 1.1 硬件与环境

| 编号 | 需要的环境 | 说明 |
| --- | --- | --- |
| A1 | Win10 x64，至少一块物理显示器 | 用于验证有显示时的安装、连接与卸载 |
| A2/A7 | Win10 x64 无头机（无物理显示器，最好是无显示输出的物理机） | A7 的 `auto` 只有在 worker 看不到任何活动显示时才生效；普通 VM 的虚拟显卡可能被算作活动显示 |
| A3 | Win11 24H2 与 25H2 x64 | 同一台机器升级或两台机器均可 |
| A4 | Win11 ARM64 | 只需运行托盘与 CLI，不需要驱动包 |
| A5 | 已安装 VDD、Parsec 或 RustDesk IddCx 驱动的机器 | 可用快照回滚的测试机 |
| A6 | 任一已安装 usbmmidd 的机器 | 需要一台查看器发起真实会话 |
| A8/A9 | 任一已安装 usbmmidd 的机器 | 可用 A1/A2 的机器继续 |
| A10 | 任一已安装 usbmmidd 的机器 | 安全模式操作，务必先做快照 |

每台测试机在开始前创建快照或系统还原点。A1、A2、A3 的安装与卸载会写入
PnP 驱动库，回滚快照比手工清理更可靠。

### 1.2 驱动包（外部前置，当前阻塞）

仓库**不包含** usbmmidd_v2 驱动包，也不会自动下载。执行 A1–A3、A5–A10 前必须
拿到 Amyuni 官方发布、经法务确认可再分发的固定哈希包，并完成以下检查：

1. 取得官方 `usbmmidd_v2.zip` 的 SHA-256，与团队评审记录一致；
2. 评审签名链、文件清单与 Amyuni 再分发条款；
3. 把评审后的哈希写入 `tools/fetch-usbmmidd.ps1` 的 `$pinnedZipSha256`，
   并同步更新 `THIRD-PARTY-NOTICES.md` 的许可全文与来源说明。

在 pin 为空时，`fetch-usbmmidd.ps1` 只会打印本地 zip 的哈希并以退出码 2 结束，
不会下载或解压。不要为了赶验收跳过这一步。

包目录必须包含：

```text
deviceinstaller64.exe
usbmmidd.inf
usbmmidd.cat
x64\usbmmidd.dll
win32\usbmmidd.dll
License.txt
manifest.sha256      # fetch 脚本生成
SOURCE.txt           # fetch 脚本生成
```

获取与打包：

```powershell
# 仅在哈希已 pin、许可已确认后执行
powershell -ExecutionPolicy Bypass -File tools\fetch-usbmmidd.ps1 -SourceZip D:\downloads\usbmmidd_v2.zip

# 生成单文件安装器（同时把驱动放进 drivers\usbmmidd_v2）
powershell -ExecutionPolicy Bypass -File tools\package-windows.ps1 `
  -Version 0.1.0 `
  -VirtualDisplayPackageDir build\deps\usbmmidd_v2
```

打包脚本会检查 `License.txt`、x64/win32 DLL、INF、CAT 与 `manifest.sha256`；
缺任何一个都会报错。没有驱动包时安装器仍可生成，但 `--virtual-display`
安装会被拒绝（CLI 退出码 3）。

### 1.3 本机构建与包检查

```powershell
cmake --build build --config Release --target chast-host chast-service
```

证据目录统一放在 `build\accept\plat-01\A<n>\`（`build` 已被 git 忽略）：

```powershell
$root = "C:\Program Files\chast"
$pkg  = Join-Path $root "drivers\usbmmidd_v2"
$ev   = "D:\accept\plat-01\A1"
New-Item -ItemType Directory -Force $ev | Out-Null

signtool verify /pa /v "$pkg\deviceinstaller64.exe" *> "$ev\signtool-deviceinstaller64.txt"
Get-FileHash "$pkg\deviceinstaller64.exe" -Algorithm SHA256 |
  Out-File "$ev\sha256-deviceinstaller64.txt"
Get-Content "$pkg\manifest.sha256" | Out-File "$ev\manifest.sha256.txt"
```

`signtool` 来自 Visual Studio 或 Windows SDK 开发者命令行。签名链必须终止于
`Microsoft Windows Hardware Compatibility Publisher`；不是微软 WHPC 的包拒绝安装
（CLI 退出码 4）。

### 1.4 统一路径

| 内容 | 路径 |
| --- | --- |
| 机器级安装根 | `C:\Program Files\chast` |
| 驱动包 | `<安装根>\drivers\usbmmidd_v2` |
| 机器级设置 | `%ProgramData%\chast\host.ini` |
| 驱动安装记录 | `%ProgramData%\chast\virtual-display.json` |
| 提权 CLI 日志 | `%ProgramData%\chast\vdisplay.log` |
| 服务日志 | `%ProgramData%\chast\service.log` |
| worker 日志（含 `vdisplay:` 行） | `%ProgramData%\chast\worker.log` |
| 托盘的本次日志 | 仅在托盘窗口内，需截图或复制 |

### 1.5 命令行约定

以下命令在**提升的 PowerShell** 中执行；`$LASTEXITCODE` 是判定依据。

```powershell
& "$root\chast-host.exe" --vdisplay status  --log "$ev\status.log"
& "$root\chast-host.exe" --vdisplay install --package "$pkg" --count 1 --log "$ev\install.log"
& "$root\chast-host.exe" --vdisplay set-count 2 --log "$ev\set-count.log"
& "$root\chast-host.exe" --vdisplay uninstall --log "$ev\uninstall.log"
```

退出码：

| 码 | 含义 |
| --- | --- |
| 0 | 成功（包括 `owned=false` 时拒绝卸载但按成功返回） |
| 2 | 用法错误、缺少管理员权限、参数越界 |
| 3 | 包缺失、哈希或清单校验失败 |
| 4 | 签名主体不符 |
| 5 | 安装失败 |
| 6 | 安装后 60 s 内设备未出现，或数量变更时设备未响应 |
| 7 | 卸载失败，或无法保护状态目录 |
| 8 | 平台不支持，或检测到其他虚拟显示驱动而拒绝安装 |
| 9 | 数量超过 4 |

`install`/`uninstall`/`set-count` 的日志默认写到 `%ProgramData%\chast\vdisplay.log`。
`uninstall` 与 `set-count` 只使用安装根下的默认包目录，不接受 `--package`。

## 2. A1：有物理显示器的安装、连接与卸载

**环境**：Win10 x64，至少一块物理显示器；已按 §1.2 准备好安装器或构建树。

**准备**：若使用构建树而非安装包，把驱动包放到
`build\Release\drivers\usbmmidd_v2`，托盘用 `build\Release\chast-host.exe`。

**步骤**：

1. 采集基线：

   ```powershell
   & "$root\chast-host.exe" --vdisplay status --log "$ev\status-before.log"
   Get-PnpDevice -Class Display | Format-List FriendlyName,InstanceId,Status |
     Out-File "$ev\display-before.txt"
   pnputil /enum-drivers | Out-File "$ev\drivers-before.txt"
   ```

   期望：`status` 含 `state=not_installed`，退出码 0。

2. 启动托盘 `chast-host.exe`。右键菜单应显示“虚拟显示器：未安装”，且
   “安装虚拟显示器…”可点击（包目录存在时）。

3. 点击“安装虚拟显示器…”，在 UAC 中确认。等待成功提示（最长约 5 分钟）。

4. 采集安装证据：

   ```powershell
   & "$root\chast-host.exe" --vdisplay status --log "$ev\status-after-install.log"
   Get-PnpDevice -Class Display | Format-List FriendlyName,InstanceId,Status |
     Out-File "$ev\display-after-install.txt"
   pnputil /enum-drivers | Out-File "$ev\drivers-after-install.txt"
   Get-Content "$env:ProgramData\chast\virtual-display.json" |
     Out-File "$ev\record.json"
   icacls "$env:ProgramData\chast" | Out-File "$ev\icacls-state-dir.txt"
   icacls "$env:ProgramData\chast\virtual-display.json" |
     Out-File "$ev\icacls-record.txt"
   ```

5. 从另一台机器用查看器连接本机，确认远程画面里能看到新增的虚拟显示器，
   把窗口拖到该显示器上，测试鼠标点击/拖动坐标是否落点正确。

6. 断开查看器。托盘右键 →“卸载虚拟显示器…”，确认提示。

7. 采集卸载证据：重复第 4 步，保存为 `status-after-uninstall.log`、
   `display-after-uninstall.txt`、`drivers-after-uninstall.txt`；
   确认 `virtual-display.json` 已删除。

**通过标准**：

- 安装成功提示后 `status` 为 `state=active provider=usbmmidd active=1 max=4`；
- `Get-PnpDevice -Class Display` 出现 `USB Mobile Monitor Virtual Display`；
- 查看器能看到并使用该显示器，输入坐标正确；
- 卸载后 `status` 回到 `state=not_installed`，设备消失；
- `pnputil /enum-drivers` 中不再有 usbmmidd 条目；
- `vdisplay.log` 至少包含 `installed usbmmidd with 1 active monitor(s)` 与
  `the usbmmidd driver was removed`；
- `%ProgramData%\chast` 的 DACL 为 SYSTEM/Administrators 完全控制、Users 只读。

**注意**：安装根下的 `drivers\usbmmidd_v2` 目录是安装器负载，卸载驱动后仍会保留；
“无残留驱动包”指 PnP 驱动库与设备实例，不是这个目录。

## 3. A2：无头机安装与重启后远程连接

**环境**：Win10 x64 无头机（无物理显示器）；构建出的单文件安装器。

**步骤**：

1. 创建快照。以提升权限运行：

   ```powershell
   .\chast-0.1.0-setup.exe --machine --service --virtual-display --virtual-display-count 1
   ```

   期望退出码 0，`sc.exe query chast-host` 显示 `RUNNING`。

2. 采集安装证据（同 A1 第 4 步），另存服务信息：

   ```powershell
   sc.exe qc chast-host | Out-File "$ev\service.txt"
   Get-Content "$root\drivers\usbmmidd_v2\SOURCE.txt" | Out-File "$ev\source.txt"
   ```

3. 服务模式没有永久密码时会拒绝所有查看器。设置永久密码：

   ```powershell
   Stop-Service chast-host
   & "$root\chast-host.exe" --config "$env:ProgramData\chast\host.ini"
   ```

   在托盘的“安全设置”里设置永久密码（≥8 字符），保存并退出托盘，然后：

   ```powershell
   Start-Service chast-host
   ```

   确认 `%ProgramData%\chast\host.ini` 含 `password_hash=pbkdf2-sha256$...`。

4. 在 `%ProgramData%\chast\host.ini` 中设置无头策略（以管理员编辑）：

   ```ini
   virtual_display=auto
   virtual_display_monitors=1
   virtual_display_resolution=1920x1080
   virtual_display_provider=usbmmidd
   ```

5. 重启测试机。

6. 检查 worker 日志：

   ```powershell
   Get-Content "$env:ProgramData\chast\worker.log" |
     Select-String "vdisplay:" | Out-File "$ev\worker-vdisplay.txt"
   ```

   在无头机上期望看到
   `vdisplay: virtual display ready: 1 monitor(s) at 1920x1080 [state=active, monitors=1]`。
   如果机器仍被识别为有活动显示，会看到
   `vdisplay: a display is already active; leaving the topology unchanged`；
   这仍可完成远程连接验收，但 `auto` 的“无显示时启用”要在 A7 的专用环境验证。

7. 用查看器连接，确认登录后有可操作桌面、鼠标坐标正确。

**通过标准**：安装退出 0；重启后服务自动运行；`status` 为 `active` 或至少
`installed` 且远程桌面可用；worker 日志有对应的 `vdisplay:` 行；查看器可操作。

## 4. A3：Win11 24H2/25H2

在 Win11 24H2 和 25H2 x64 上分别重复 A1 与 A2。额外采集：

```powershell
Get-WinEvent -LogName System -MaxEvents 300 |
  Where-Object { $_.ProviderName -match 'Kernel-PnP|Wudf|Display' } |
  Out-File "$ev\system-pnp-events.txt"
```

记录驱动版本（本包为 `2.0.0.1`）、是否有 Code 43/52、是否要求重启。
Win11 专属差异写入 `docs/status-2026-10-25.md` 的验收小节。

## 5. A4：Win11 ARM64

**环境**：Win11 ARM64；ARM64 构建的 `chast-host.exe`；不需要驱动包。

**步骤**：

1. 启动托盘，右键查看虚拟显示器状态。
2. 运行：

   ```powershell
   & "$root\chast-host.exe" --vdisplay status --log "$ev\status-arm64.log"
   "exit=$LASTEXITCODE" | Out-File "$ev\exit.txt"
   & "$root\chast-host.exe" --vdisplay install --package "$pkg" --count 1 `
       --log "$ev\install-arm64.log"
   "exit=$LASTEXITCODE" | Out-File -Append "$ev\exit.txt"
   ```

**通过标准**：

- 托盘显示“虚拟显示器：不支持”，没有任何安装入口；
- 两条命令都退出 8；
- 日志含 `unsupported: usbmmidd ships an x64 driver only; ARM64 is not supported`；
- 没有任何设备或驱动被安装。

## 6. A5：已装其他虚拟显示驱动

**环境**：测试机已安装 VDD、Parsec 或 RustDesk 的虚拟显示驱动；准备 usbmmidd 包。

**步骤**：

1. 启动托盘，确认状态为“虚拟显示器：检测到其他虚拟显示驱动”，安装项灰显。
2. 直接运行提权 CLI 尝试安装：

   ```powershell
   & "$root\chast-host.exe" --vdisplay install --package "$pkg" --count 1 `
       --log "$ev\install-conflict.log"
   "exit=$LASTEXITCODE" | Out-File "$ev\exit.txt"
   ```

3. 采集现有驱动列表：

   ```powershell
   pnputil /enum-drivers | Out-File "$ev\drivers-before.txt"
   Get-PnpDevice -Class Display | Format-List FriendlyName,InstanceId,Status |
     Out-File "$ev\display-conflict.txt"
   ```

**通过标准**：

- 托盘不提供可点击的安装入口；
- CLI 退出 8，日志含
  `install refused: another virtual display driver is present: <hardware-id>`；
- 安装前后 `pnputil /enum-drivers` 与显示设备列表没有新增 usbmmidd 条目；
- 不出现两个虚拟显示驱动叠加。

## 7. A6：会话进行中卸载

**环境**：已安装 usbmmidd 的机器；一台查看器保持连接。

**步骤**：

1. 查看器连接并保持 Streaming。
2. 托盘右键 →“卸载虚拟显示器…”。
3. 观察菜单项和对话框。

**通过标准**：

- 会话进行中，托盘菜单显示灰显的“卸载虚拟显示器（请先结束会话）”；
- 即使通过快捷方式触发处理函数，也只弹出“请先结束远程会话，再卸载虚拟显示器。”，
  不会执行卸载；
- 结束会话后，菜单恢复为可点击的“卸载虚拟显示器…”，可以正常卸载。

**已知边界**：服务安装器的卸载会先停止服务，因此不会在会话中卸载；但如果
管理员在会话进行中直接运行 `chast-host.exe --vdisplay uninstall`，CLI 本身没有
会话感知，设备移除会失败并返回 7。该路径不在支持的操作流程内，A6 只对托盘
入口做验收；若需要 CLI 也强制拦截，另立任务实现跨进程会话标记。

## 8. A7：重启持久性与 off/auto

**环境**：A2 的无头机；能够确认 worker 看不到任何活动显示（否则 `auto` 不会触发）。

**步骤**：

1. 先关闭所有虚拟显示器（保持驱动安装）：

   ```powershell
   & "$root\chast-host.exe" --vdisplay set-count 0 --log "$ev\set-count-0.log"
   ```

2. 把 `%ProgramData%\chast\host.ini` 设为 `virtual_display=off`，重启。
3. 采集 `worker.log` 与 `status`，确认 worker 没有创建或删除虚拟显示器：
   `off` 时 `prepare_virtual_display_on_start` 直接返回，不产生 `vdisplay:` 行。
4. 把设置改为 `virtual_display=auto`，再次重启。
5. 采集 `worker.log`，在无显示环境下期望
   `vdisplay: virtual display ready: 1 monitor(s) at ...`；随后 `status` 为 `active`。

**通过标准**：

- `off`：重启后拓扑不变，`active=0`，没有自动启用；
- `auto`：无活动显示时自动出现 1 台虚拟屏；若机器始终被判定为有活动显示，
  记录 `a display is already active; leaving the topology unchanged`，该项标记为
  “环境不具备”，不要算通过；
- 两种策略都不修改主显示器、DPI 或用户已有排列。

## 9. A8：4 台虚拟屏与第 5 台上限

**步骤**：

1. 在已安装驱动的机器上：

   ```powershell
   & "$root\chast-host.exe" --vdisplay set-count 4 --log "$ev\set-count-4.log"
   & "$root\chast-host.exe" --vdisplay status --log "$ev\status-4.log"
   & "$root\chast-host.exe" --vdisplay set-count 5 --log "$ev\set-count-5.log"
   "exit=$LASTEXITCODE" | Out-File "$ev\exit-5.txt"
   ```

2. 在托盘“安全设置 → 高级”把数量改为 5 并保存，观察提示。
3. 在 `%ProgramData%\chast\host.ini` 写入 `virtual_display_monitors=5`，
   重启 worker，检查日志告警与实际数量。

**通过标准**：

- 4 台全部出现，`status` 为 `active=4 max=4`；
- `set-count 5` 退出 2，日志含 `set-count must be from 0 to 4`；
- 托盘拒绝 5 并提示 1–4；
- 配置写 5 时产生“exceeds 4”告警并钳制为 4，不崩溃。

## 10. A9：不支持的分辨率回退

**步骤**：

1. 在 `%ProgramData%\chast\host.ini` 设置 `virtual_display_resolution=2560x1440`，
   重启 worker，检查 `worker.log` 中选择的实际模式。
2. 若 2560×1440 在实时模式列表中，改用 `1919x1080` 再试一次，强制触发最近匹配。

**通过标准**：

- 不崩溃，虚拟显示器正常启用；
- worker 日志显示 `vdisplay: ... at <实际宽>x<实际高>`；
- 实际模式是列表中与请求最接近的模式；请求模式存在时精确匹配。

## 11. A10：安全模式卸载

**环境**：已安装 usbmmidd 的机器；先做快照。

**进入安全模式**（二选一）：

```powershell
# 方式一：一次性安全模式
bcdedit /set {current} safeboot minimal
Restart-Computer
```

或 `msconfig` → 引导 → 安全引导 → 最小。

**安全模式中执行**：

```powershell
$root = "C:\Program Files\chast"
& "$root\chast-host.exe" --vdisplay uninstall --log "$env:TEMP\vdisplay-safe.log"
"exit=$LASTEXITCODE"
Get-Content "$env:TEMP\vdisplay-safe.log"
pnputil /enum-drivers | Select-String -Pattern "usbmmidd"
```

期望：CLI 退出 0；日志含 `the usbmmidd driver was removed`；`pnputil` 无 usbmmidd。
如果 CLI 因状态目录/包校验失败（退出 5/7），改用设备管理器 → 显示适配器 →
卸载“USB Mobile Monitor Virtual Display”，勾选“删除此设备的驱动程序软件”。

**退出安全模式并恢复**：

```powershell
bcdedit /deletevalue {current} safeboot
Restart-Computer
```

**通过标准**：安全模式中驱动被移除；正常重启后系统显示恢复正常；`status` 为
`state=not_installed`；`pnputil /enum-drivers` 无 usbmmidd。

## 12. 证据归档

每项完成后在 `docs/status-2026-10-25.md` 增加一个小节，至少包含：

- 测试机信息（Windows 版本/架构、物理或虚拟、是否有无头环境）；
- `signtool verify /pa /v` 的签名链摘要与 `manifest.sha256` 校验结果；
- `pnputil /enum-drivers`、`Get-PnpDevice -Class Display` 的前后对比；
- `vdisplay.log` 与 `worker.log` 中的 `vdisplay:` 行（原样粘贴关键行）；
- `virtual-display.json` 内容与 `icacls` 输出；
- 查看器截图/录屏（无头主机远程桌面）；
- 卸载后设备、驱动库与状态文件的检查结果；
- 结论：通过 / 不通过 / 环境不具备，以及原因。

`build/accept/plat-01/` 下的原始文件不提交到 git；状态文档只记录可复核的文本
和哈希。

## 13. 结果汇总

执行时填写下表并保留在状态文档中：

| 编号 | 测试机 | 日期 | 结果 | 备注 |
| --- | --- | --- | --- | --- |
| A1 | | | 未执行 | |
| A2 | | | 未执行 | |
| A3 | | | 未执行 | 分 24H2/25H2 两行 |
| A4 | | | 未执行 | |
| A5 | | | 未执行 | |
| A6 | | | 未执行 | 托盘入口已加会话拦截 |
| A7 | | | 未执行 | 需真正无活动显示的环境 |
| A8 | | | 未执行 | |
| A9 | | | 未执行 | |
| A10 | | | 未执行 | 需快照 |

## 14. 当前代码与验收的差距

| 编号 | 差距 | 影响 | 处理 |
| --- | --- | --- | --- |
| G1 | 固定哈希包、Amyuni 再分发许可与许可全文未就绪 | 不能合法打包/分发驱动，A1–A3、A5–A10 阻塞 | 外部评审；pin 前不得下载 |
| G2 | 专用无头/多系统测试机未提供 | A2/A3/A7/A10 无法执行 | 安排测试机与快照 |
| G3 | CLI `--vdisplay uninstall` 不感知活动会话 | 直接命令行卸载可能失败并返回 7，而非友好拒绝 | 已记录；如需强制，另立跨进程会话标记任务 |
| G4 | `auto` 依赖 `enumerate_displays()` 判断“无显示” | 虚拟机的虚拟显卡可能被视为活动显示 | 在真正的无头物理机执行 A7；否则记录“环境不具备” |
| G5 | `owned=false` 时不卸载 | 第三方预装的 usbmmidd 只复用、不认领 | 符合设计；A1 只验收 chast 自己安装的驱动 |

完成本手册前，PLAT-01b/01d 不能标记为完成，PLAT-01e 也不能开始。
