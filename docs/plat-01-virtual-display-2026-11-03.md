# PLAT-01 虚拟显示器：选型与接入设计（usbmmidd_v2）

- 状态：PLAT-01a/01c 已落地；PLAT-01b/01d 的代码与本地接线已完成，
  真机安装仍被 Amyuni 再分发许可、固定哈希驱动包和测试机阻塞
- 日期：2026-11-03
- 关联：`docs/rustdesk-gap-tasks-2026-11-01.md` 的 PLAT-01
- 目标：让没有接显示器的 Windows 主机也能被 chast 查看器远程操作，并且安装、卸载和签名都可验证。

## 1. 结论

1. **第一版采用 Amyuni `usbmmidd_v2`**，作为唯一实现的虚拟显示提供方。
   - 它是微软 WHQL 签名的驱动包，安装时不会出现“未知发布者”或证书信任警告，直接满足 PLAT-01 的“驱动签名有效”验收项。
   - RustDesk 目前也走这条路：放弃自研 IddCx 驱动和证书，随安装包分发 `usbmmidd_v2`。
2. **Virtual-Display-Driver（VDD/MttVDD）暂不集成**，只保留检测和预留接口。
   - 它是 MIT 开源、功能更强（HDR、自定义 EDID、动态控制），但只有 SignPath Authenticode 签名，x64 需要把第三方证书加入受信任发布者，ARM64 24H2+ 必须开测试签名，不能作为第一版默认方案。
3. **自研 IddCx + WHQL 驱动继续留在长期路线**，用于 ARM64、企业分发和超过 4 屏的场景；第一版不为它投入。
4. **驱动安装必须发生在管理员和本地显示都还在的时候**。真正的无头主机没有本地界面去点 UAC，所以推荐在 `chast-setup --machine --service --virtual-display` 安装阶段完成驱动安装；托盘里的“安装虚拟显示器”只作为有显示时的补充入口。

## 2. 范围

### 2.1 本版包含

- x64 Windows 10/11 主机；
- 1～4 台虚拟显示器，默认 1 台、1920×1080；
- 驱动检测、安装、卸载、启用、停用、数量调整；
- 无物理显示器时自动启用（需操作员显式打开）；
- 主机托盘开关、`host.ini` 配置、日志和错误提示；
- 安装包可选组件、第三方许可声明、卸载清理；
- 单元测试和一台专用 x64 无头机器的实机验收。

### 2.2 本版不包含

- ARM64 正式支持（usbmmidd 没有 ARM64 二进制）；
- HDR、10-bit、自定义 EDID、分数刷新率；
- 超过 4 台虚拟显示器；
- 查看器侧发起“增加/删除虚拟屏”的协议消息（留到后续协议版本）；
- 自研驱动或 WHQL 提交；
- 自动修改主显示器、DPI 缩放或显示排列（只做必要的扩展定位）。

## 3. 选型依据（2026-11-03 实测）

### 3.1 签名与版本

从 Amyuni 下载的 `usbmmidd_v2.zip`（199,309 字节，SHA-256
`629b51e9944762bae73948171c65d09a79595cf4c771a82ebc003fbba5b24f51`）解包后用
`signtool verify /pa /v` 验证：

| 文件 | 签名链 | 时间戳 |
| --- | --- | --- |
| `usbmmIdd.cat` / `x64/usbmmIdd.dll` | Microsoft Root Certificate Authority 2010 → Microsoft Windows Third Party Component CA 2014 → **Microsoft Windows Hardware Compatibility Publisher** | 2021-09-09 |

INF 关键字段：

```
Class = Display
ClassGUID = {4D36E968-E325-11CE-BFC1-08002BE10318}
DriverVer = 08/31/2021, 2.0.0.1
CatalogFile = usbmmIdd.cat
UmdfExtensions = IddCx0102
```

驱动包内文件哈希（用于运行时校验和升级时对照）：

| 文件 | SHA-256 |
| --- | --- |
| `usbmmIdd.cat` | `470149c4cf9970ba59070aa7c9409c9f63a15727de99bab53e7e51f55310779f` |
| `usbmmIdd.inf` | `a8750ca15a86742f3012886c9932bb974158cd2d9779cf891c730d976a47726a` |
| `x64/usbmmIdd.dll` | `dc135d675127113915a7e5aa9fe57c84edad6be41d0890b265ef124ab26ea9e3` |
| `deviceinstaller64.exe` | `e9baa02cdae921acf0aae4d8e8c29a4cdf4057ab61f9c60862b7cc439e2753f7` |
| `License.txt` | `44c73fc4110979899dbb5054662de581f7d4350939f19bb2d50574f613314ba2` |

这些哈希对应 2026-11-03 从 Amyuni 下载的版本。Amyuni 若更新包，必须人工复核并更新固定值，不能静默接受新包。

### 3.2 与 VDD 的取舍

| 维度 | usbmmidd_v2 | VDD（MttVDD） |
| --- | --- | --- |
| 签名 | 微软 WHQL，安装无额外信任步骤 | SignPath Authenticode，需要信任第三方发布者证书 |
| 开源 | 闭源 | MIT |
| 平台 | x64；无 ARM64 | x64；ARM64 24H2+ 需测试签名 |
| 显示器数量 | 最多 4 | 可配置，VDC 可动态增删 |
| 分辨率/刷新率 | 默认列表，注册表最多 10 个，最高 3840×2160 | 自定义分辨率、8K、分数刷新率 |
| HDR/EDID/光标 | 不支持 | 支持 |
| 控制方式 | `deviceinstaller64 enableidd 0/1` | 命名管道 `\\.\pipe\MTTVirtualDisplayPipe` + `vdd_settings.xml` |
| 维护状态 | 2021 年后停更 | 2025 年仍活跃 |
| 主要风险 | 功能受限、许可含广告提示条款 | 证书信任、GPU 驱动更新后黑屏、ARM64 限制 |

结论：第一版选 usbmmidd 是因为**部署摩擦最低**，而不是功能最强。若后续需要 HDR、自定义 EDID、超过 4 屏或程序化控制，再增加 VDD provider，不改动上层接口。

## 4. 产品行为

### 4.1 host.ini 配置

新增以下键（沿用 `HostSettings` 的平面 key=value 和逐项回退风格）：

```ini
# off：不自动管理；auto：无活动显示时在会话启动前启用；on：总是启用
virtual_display=off
# 1..4，默认 1；超过 4 回退到 4 并告警
virtual_display_monitors=1
# 首选分辨率，格式 WxH；不支持时选最接近的模式并记录日志
virtual_display_resolution=1920x1080
# auto：检测已安装的提供方；第一版只实现 usbmmidd
virtual_display_provider=auto
```

语义：

- `off`（默认）：不自动启用/停用；托盘的“启用/停用”仍然可用。
- `auto`：在会话 worker 启动、`HostServer` 开始采集之前检查；如果 `enumerate_displays()`
  没有任何有面积的活动显示，则确保虚拟显示器按 `virtual_display_monitors` 启用。
  有物理显示时不动作。**会话运行期间不删除虚拟显示器**，避免画面和窗口被重新排列；
  虚拟显示器保持到下次启动或操作员手动停用。
- `on`：无论是否有物理显示，都在 worker 启动时确保启用。用于测试和多屏场景。
- 不自动修改主显示器、DPI 或显示排列；虚拟显示器作为扩展屏放在现有桌面右侧。
- 驱动已安装但 `virtual_display=off` 时，不改变当前显示拓扑。

### 4.2 托盘 UI

在“访问”或“高级”设置页增加：

- 复选框“无显示器时自动启用虚拟显示器” → `virtual_display=auto`；
- 复选框“总是启用虚拟显示器” → `virtual_display=on`（可与上项合并为三选一）；
- 数字输入“虚拟显示器数量（1～4）” → `virtual_display_monitors`；
- 下拉框“虚拟显示器分辨率” → `virtual_display_resolution`（从驱动支持列表读取）；
- 按钮“安装驱动 / 卸载驱动”，显示当前状态（未安装、已安装、运行中、不支持）；
- 状态行显示提供方（usbmmidd / VDD）、活动数量、最后错误。

安装/卸载按钮必须给出明确确认框，并显示 Amyuni 许可提示。卸载在会话进行中时拒绝，提示先结束会话。

### 4.3 无头部署的关键约束

- 驱动安装需要管理员权限。无头主机没有本地界面，UAC 无法在远程会话里点确认。
- 因此推荐流程是：在还有本地显示时运行
  `chast-setup.exe --machine --service --virtual-display`，由安装器一次性完成驱动安装；
  之后即使拔掉显示器，worker 启动时也能按 `virtual_display=auto` 启用。
- 托盘安装入口只用于“现在还有显示、但以后会无头”的场景；不承诺在完全无头且没有管理员在场时远程安装驱动。

## 5. 架构与接口

### 5.1 新增模块

| 文件 | 作用 |
| --- | --- |
| `include/chast/virtual_display.h` | provider 接口、状态、模式、配置与错误类型 |
| `src/host/virtual_display.cc` | provider 检测、usbmmidd 实现、状态文件、模式选择 |
| `tests/virtual_display_test.cc` | 纯逻辑单测（不安装驱动） |
| `host/platform.cc` | 复用 `request_firewall_rule` 的提权模式，增加虚拟显示动作 |
| `host/main.cc` | `--vdisplay ...` 隐藏 CLI、托盘 UI 接线 |
| `host/worker.cc` | worker 启动前执行 `virtual_display_prepare(settings)` |
| `src/host/host_settings.cc` | 新增键的解析、校验和序列化 |
| `tools/fetch-usbmmidd.ps1` | 下载并校验固定哈希，产出 `build/deps/usbmmidd_v2` |
| `tools/package-windows.ps1` | 把驱动包复制进安装负载 |
| `server/cmd/chast-setup/main.go`、`server/internal/setup/*.go` | `--virtual-display` 安装/卸载选项 |

`chast.host` 的 CMake 源列表需要加入 `src/host/virtual_display.cc`；
`chast.host.tray` 需要链接 SetupAPI（`setupapi`）和 Wintrust（`wintrust`）。

### 5.2 Provider 接口

```cpp
namespace chast {

enum class VirtualDisplayProviderKind { None, Usbmmidd, Vdd };
enum class VirtualDisplayState { Unsupported, NotInstalled, Installed, Active, Error };

struct VirtualDisplayMode {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refresh_hz = 0;
};

struct VirtualDisplayStatus {
    VirtualDisplayProviderKind provider = VirtualDisplayProviderKind::None;
    VirtualDisplayState state = VirtualDisplayState::Unsupported;
    uint32_t active_monitors = 0;
    uint32_t max_monitors = 0;
    std::string detail;  // 面向用户的 UTF-8 原因
};

class VirtualDisplayProvider {
  public:
    virtual ~VirtualDisplayProvider() = default;
    virtual VirtualDisplayProviderKind kind() const = 0;
    virtual VirtualDisplayStatus status() const = 0;
    virtual std::vector<VirtualDisplayMode> modes() const = 0;
    virtual bool install(std::string* error) = 0;
    virtual bool uninstall(std::string* error) = 0;
    virtual bool set_monitor_count(uint32_t count, std::string* error) = 0;
    virtual bool select_mode(const VirtualDisplayMode& mode, std::string* error) = 0;
};

VirtualDisplayProviderKind detect_virtual_display_provider();
std::unique_ptr<VirtualDisplayProvider> make_virtual_display_provider(
    VirtualDisplayProviderKind kind, const std::wstring& package_dir);
bool virtual_display_platform_supported(std::string* reason);

}  // namespace chast
```

第一版 `make_virtual_display_provider` 只接受 `Usbmmidd`；`Vdd` 返回 `nullptr`
并在 `detail` 里写“尚未实现”。检测到 VDD/Parsec/RustDeskIddDriver 时返回
`None`，由托盘提示“检测到其他虚拟显示驱动，建议停用后再安装”，不尝试共存。

### 5.3 usbmmidd 实现要点

- **检测**
  - SetupAPI 枚举 `GUID_DEVCLASS_DISPLAY` 的 present 设备，按硬件 ID `usbmmidd`
    或服务名 `usbmmIdd` 匹配，不能用本地化的友好名称。
  - 同时检查 `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\usbmmIdd`
    是否存在，用于区分“已安装但未启用”和“未安装”。
  - 数量以 present 且状态正常的显示设备数为准。
- **安装**
  - 校验包内文件哈希和 `usbmmidd.cat` 的签名主体必须是
    `Microsoft Windows Hardware Compatibility Publisher`；任一失败直接拒绝。
  - 以包目录为工作目录运行
    `deviceinstaller64.exe install usbmmidd.inf usbmmidd`。
  - `deviceinstaller64` 的安装是异步的：安装返回后轮询设备出现，
    每 250 ms 一次、最多 60 s；超时视为失败并执行清理。
- **启用/停用**
  - `deviceinstaller64 enableidd 1` 每次增加一台，重复调用直到达到目标数量；
    `enableidd 0` 每次移除一台。
  - 目标数量限制 0～4；增加/减少后轮询设备数量，最多 30 s。
  - 部分成功时保留已经出现的显示器，返回实际数量并记录错误；
    只要至少有一台可用，不阻止会话启动。
- **模式与排列**
  - `modes()` 从 `EnumDisplaySettings` 读取虚拟显示器的可用模式，并与驱动
    默认列表求交集；不使用未经验证的自定义模式。
  - `select_mode()` 用 `ChangeDisplaySettingsEx` 应用到第一台虚拟显示器；
    其他虚拟显示器使用驱动默认模式（v1 限制）。
  - 用 `SetDisplayConfig` 把虚拟显示器扩展到现有桌面右侧；
    不改变主显示器，不修改 DPI。
  - 首选分辨率不在可用列表时，选择宽高比最接近的模式并写日志。
- **卸载**
  - 先 `deviceinstaller64 stop usbmmidd`，再
    `deviceinstaller64 remove usbmmidd`；轮询设备消失，最多 60 s。
  - 有活动会话时拒绝卸载；服务运行中由托盘先提示停止服务或结束会话。
  - 卸载不删除用户文件；只清理由本产品安装的包目录和状态文件。
  - 返回“需要重启”状态时，保留安装目录并提示重启后重试，不假装成功。
- **注册表**
  - v1 不修改 `...\WUDF\Services\usbmmIdd\Parameters\Monitors`。
    如果以后需要预置自定义分辨率，必须先备份原值，并在卸载时恢复；
    未备份成功则放弃修改。

### 5.4 提权与调用路径

复用 `host/platform.cc` 里 `request_firewall_rule` 的模式：隐藏启动自身，
`lpVerb = "runas"`，等待子进程退出并读取退出码。新增：

```text
chast-host.exe --vdisplay install --package <目录> --count <1..4>
chast-host.exe --vdisplay uninstall
chast-host.exe --vdisplay set-count <0..4>
chast-host.exe --vdisplay status [--log <文件>]
```

- `install` / `uninstall` / `set-count` 需要管理员，由托盘以 `runas` 调起；
- `status` 不需要提权，用于托盘刷新和诊断；
- 子进程把详细错误写到 `--log` 指定的文件（默认
  `%ProgramData%\chast\vdisplay.log`），托盘读取最后一行展示；
- 退出码：`0` 成功、`2` 参数错误或缺少管理员权限、`3` 包缺失/哈希不符、
  `4` 签名不符、`5` 安装失败、`6` 设备未出现、`7` 卸载失败、
  `8` 平台不支持、`9` 数量越界。

**服务策略**：`chast-service` 以 LocalSystem 运行，可以执行
`set-count`/启用停用，但**永远不自动安装或卸载驱动**。安装/卸载只发生在
安装器或托盘（有管理员和用户确认）路径。worker 在用户会话中运行，
负责在启动时执行 `virtual_display_prepare`，避免 Session 0 看不到交互桌面。

### 5.5 状态文件

`%ProgramData%\chast\virtual-display.json`，由提权进程写入，tray/worker 只读：

```json
{
  "provider": "usbmmidd",
  "package_version": "2.0.0.1",
  "package_sha256": "629b51e...",
  "installed_at": "2026-11-03T12:00:00Z",
  "owned": true,
  "last_error": ""
}
```

`owned` 用于区分“本产品安装的驱动”和“用户或其他软件已装的 usbmmidd”：
`owned=false` 时只复用，不卸载。状态文件损坏时按“未安装”处理并记录日志。

目录和文件都带受保护 DACL：目录为
`D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GR;;;BU)`（Users 只读），
记录文件为 `D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;BU)`，owner 为
BUILTIN\Administrators。读取方先检查目录/文件 ACL，不可信时按“无记录”处理；
写入使用同目录临时文件 + `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)` 原子
替换，见 §7。

## 6. 打包与分发

- **不把第三方二进制提交进 git**。`tools/fetch-usbmmidd.ps1` 负责获取和校验：
  - 固定哈希常量 `$pinnedZipSha256` 当前为空；未配置时脚本只打印本地 zip 的
    SHA-256 并退出码 2，不下载、不解包任何内容（fail-closed）；
  - 配置后从 `https://www.amyuni.com/downloads/usbmmidd_v2.zip` 下载，或使用
    `-SourceZip <文件>` 指定的本地包，并校验固定 SHA-256；
  - 解包到 `build/deps/usbmmidd_v2`，保留原始 `License.txt` 和
    `idd_instructions.txt`，生成 sha256sum 风格的 `manifest.sha256` 与
    `SOURCE.txt`（URL、日期、包哈希、驱动版本）；
  - 已存在且校验通过的目标目录不重复解包；加 `-Force` 才替换。
- `tools/package-windows.ps1` 增加 `-VirtualDisplayPackageDir <目录>`：
  - 省略时回退到仓库的 `drivers\usbmmidd_v2`，两者都不存在则只告警，
    安装包仍可生成，但 `--virtual-display` 会被安装器拒绝；
  - 检查必需文件齐全后复制到暂存树的 `drivers\usbmmidd_v2\`；
  - 发布前把 `dist/chast-<ver>-setup.exe` 的哈希写入发布记录。
- 安装后的目录布局：

```text
<安装根>\drivers\usbmmidd_v2\
  usbmmIdd.inf
  usbmmIdd.cat
  x64\usbmmIdd.dll
  Win32\usbmmIdd.dll
  deviceinstaller64.exe
  deviceinstaller.exe
  License.txt
  SOURCE.txt          # 下载 URL、日期、SHA-256、“未修改”
```

- **许可**：仓库根目录的 `THIRD-PARTY-NOTICES.md` 已加入 Amyuni 版权、包哈希
  和分发状态占位；安装目录保留未修改的 `License.txt`。发布前必须确认再分发
  和广告提示条款；若不允许随产品分发，退化为“用户主动下载 + 哈希校验 +
  明确确认”的引导安装，不在安装包里携带二进制。许可全文/官方链接待 Amyuni
  确认后补入。
- **安装器**：`chast-setup --machine --service --virtual-display` 在复制完负载后，
  以已提升的权限调用 `<安装根>\chast-host.exe --vdisplay install ...`；
  失败则整个安装失败并给出日志路径。卸载时先调用 `--vdisplay uninstall`，
  再删除文件；驱动卸载失败则保留文件并返回错误。

## 7. 失败处理与安全

| 场景 | 处理 |
| --- | --- |
| 包缺失或哈希不符 | 拒绝安装，提示重新获取安装包 |
| 签名主体不是微软 WHPC | 拒绝安装，记录实际签名主体 |
| 设备安装成功但 60 s 内未出现 | 自动执行卸载清理，报告“设备未出现” |
| 目标数量超过 4 | 回退到 4，托盘提示；不静默忽略 |
| 启用后只有部分显示器出现 | 保留可用部分，记录实际数量 |
| 卸载时有活动会话 | 拒绝并提示先结束会话 |
| 卸载返回需要重启 | 保留文件，提示重启后重试 |
| 检测到 VDD/Parsec/RustDesk 驱动 | 不安装，提示冲突；允许用户手动停用后重试 |
| ARM64 或非 Windows | 状态 `Unsupported`，隐藏安装入口 |
| 安装后黑屏/显示优先级错乱 | 提供安全模式说明和 `--vdisplay uninstall` CLI；见下 |

黑屏恢复说明（写入安装指南和托盘帮助）：

1. 强制关机 2～3 次进入 Windows 恢复环境；
2. 疑难解答 → 高级选项 → 启动设置 → 重启 → 安全模式；
3. 设备管理器 → 显示适配器 → 卸载 “USB Mobile Monitor Virtual Display”；
4. 或在安全模式运行 `<安装根>\chast-host.exe --vdisplay uninstall`；
5. 正常重启。

其他安全约束：

- 安装/卸载必须由操作员显式确认，不能在后台静默执行；
- 服务不得安装驱动，只能启停已经安装的驱动；
- 不修改主显示器、DPI 和用户现有显示排列；
- 不下载并执行任何非固定哈希的驱动包；
- 安装前检查系统架构、Windows 版本和已有虚拟显示驱动；
- 包目录、manifest、全部清单文件和 `SOURCE.txt` 在哈希/签名校验与
  `deviceinstaller64` 运行期间以只读共享句柄持续锁定，持有
  `VirtualDisplayPackageGuard` 时写入、删除、改名和目录改名都会失败，避免
  校验后替换（TOCTOU）；任何 reparse point、错误对象类型或允许非
  SYSTEM/Administrators/TrustedInstaller/当前用户写入的 ACL 都拒绝安装；
- `%ProgramData%\chast` 与 `virtual-display.json` 使用受保护 DACL
  （SYSTEM/Administrators 完全控制、Users 只读，owner 为
  BUILTIN\Administrators）；记录经临时文件原子替换并读回验证，ACL 不可信时
  按无记录处理，日志文件拒绝 reparse point。

## 8. 测试与验收

### 8.1 单元测试（无需驱动，CI 可跑）

- `virtual_display` 配置解析/序列化：默认值、`auto/on/off`、数量 0/1/4/5、
  非法分辨率、旧配置缺键；
- 模式选择：精确匹配、宽高比最接近、无匹配时回退默认；
- provider 选择：仅 usbmmidd 可用、VDD 未实现、检测到冲突、ARM64；
- 状态机：未安装 → 已安装 → 运行中 → 停用 → 卸载；
- 哈希校验： fixture 正确/篡改/缺失；
- 包 ACL/TOCTOU：签名包校验后句柄保持锁定（写入/删除失败，`reset()` 后
  恢复）、目录 reparse point 拒绝、manifest 路径逃逸拒绝；
- 状态目录：受保护 DACL 信任、记录原子替换与读回、无临时文件残留、
  Users 可写记录被丢弃、CREATOR OWNER 旧记录迁移后保留、裸目录拒绝写入；
- 提权参数构造：带空格的安装路径、数量边界、动作白名单；
- 状态文件 `owned=false` 时不得卸载。

### 8.2 实机验收矩阵

| 编号 | 环境 | 步骤 | 通过标准 |
| --- | --- | --- | --- |
| A1 | Win10 x64，有物理显示器 | 托盘安装 → 启用 1 台 → 连接 → 卸载 | 设备出现、远程画面正常、卸载后设备消失、无残留驱动包 |
| A2 | Win10 x64，无物理显示器 | `chast-setup --machine --service --virtual-display` → 重启 → 远程连接 | 主机启动后自动出现虚拟屏，查看器可操作，输入坐标正确 |
| A3 | Win11 24H2/25H2 x64 | 同 A1/A2 | 同上；记录驱动兼容性问题 |
| A4 | Win11 ARM64 | 打开托盘设置 | 显示“不支持”，不出现安装按钮，不尝试安装 |
| A5 | 已装 VDD/Parsec 的机器 | 尝试安装 | 明确提示冲突，不静默叠加驱动 |
| A6 | 会话进行中 | 尝试卸载 | 拒绝并提示先结束会话 |
| A7 | 重启持久性 | 设置 `virtual_display=off/auto` 各重启一次 | `off` 不自动改拓扑；`auto` 在无显示时启用 |
| A8 | 4 台虚拟屏 | 请求 4 台，再请求第 5 台 | 4 台正常；第 5 台被拒绝并提示上限 |
| A9 | 不支持的分辨率 | 选择 2560×1440（若不在列表） | 选择最接近模式并记录日志，不崩溃 |
| A10 | 安全模式 | 执行 `--vdisplay uninstall` | 驱动移除，系统恢复正常显示 |

逐项的前置条件、命令、期望输出、证据路径与回滚步骤见
`docs/plat-01-acceptance-runbook-2026-11-04.md`。

### 8.3 证据归档

每项完成后在 `docs/status-2026-10-25.md` 增加小节，至少包含：

- `signtool verify /pa /v` 的签名链输出；
- `pnputil /enum-drivers` 或 `Get-PnpDevice -Class Display` 的相关条目；
- 主机日志中的 `vdisplay:` 行（检测、安装、数量、模式、错误）；
- 查看器截图或录屏（无头主机远程桌面）；
- 卸载后的设备与驱动包检查结果。

### 8.4 当前环境阻塞

本机是 Windows 10 x64、单显示器，且没有无头测试机；安装显示驱动会改变本机
显示拓扑，不能在开发机上随意执行。因此：

- 8.1 可以立即做；
- A1 需要管理员确认并接受显示拓扑变化；
- A2～A10 需要一台专用测试机或虚拟机，属于 PLAT-01 的实机验收阻塞项，
  与 ENV 系列的真实环境验收一起安排。

## 9. 实施切片

| 切片 | 内容 | 依赖 | 完成标志 |
| --- | --- | --- | --- |
| PLAT-01a | 配置键、provider 接口、检测、托盘状态展示、单测 | 无 | 不安装驱动也能正确识别“未安装/已安装/不支持/冲突” |
| PLAT-01b | 固定哈希包、提权 CLI、安装/卸载、状态文件、许可声明 | 再分发条款确认 | A1 通过，安装日志和签名证据归档 |
| PLAT-01c | 启用/停用、数量、模式选择、`auto` 策略、worker 接线 | PLAT-01b | A2、A7、A8、A9 通过 |
| PLAT-01d | `chast-setup --virtual-display`、卸载清理、第三方声明、文档 | PLAT-01b | A2 安装路径可用，卸载无残留 |
| PLAT-01e | Win11 24H2/25H2 与无头实机验收 | 专用测试机 | A2/A3/A10 全部通过，status/roadmap 更新 |

编号说明：roadmap 当前最后一条是 121（P2-113），这些切片落地时按顺序分配
P2-114 起的编号；在未落地前只使用 PLAT-01a～e，避免占用未来编号。

截至 2026-11-04：PLAT-01a、PLAT-01c 已完成；PLAT-01b 的代码脚手架、
`tools/fetch-usbmmidd.ps1` 骨架与 `THIRD-PARTY-NOTICES.md` 占位，以及
PLAT-01d 的安装器/托盘/打包本地接线均已完成。固定哈希驱动包、Amyuni 再
分发许可、许可全文与真机 A1/A2 仍阻塞，因此 PLAT-01b/01d 不能标记为完成。

### 9.1 落地记录

**PLAT-01a 已完成（2026-11-03）**

- `host.ini` 新增 `virtual_display`、`virtual_display_monitors`、
  `virtual_display_resolution`、`virtual_display_provider`，含范围校验、
  超限钳制与逐项告警；`tests/host_settings_test.cc` 覆盖往返、默认值与非法值。
- 新增 `include/chast/virtual_display.h`、`src/host/virtual_display.cc`：
  provider 接口、x64/Windows 10 1809 平台检查、SetupAPI 检测 usbmmidd 适配器
  与显示器子设备、WUDF 服务键、VDD/Parsec/RustDesk 冲突检测、状态分类与
  模式选择；`tests/virtual_display_test.cc` 覆盖分类、模式选择与工厂。
- 托盘菜单和“安全设置 → 高级”页增加只读状态行（不支持/未安装/已安装/
  运行中/冲突），中英文文案同步。
- 验证：`ctest -C Release -R "host_settings|virtual_display|host_i18n"`
  3/3 通过；`chast-host`、`chast-service` 构建通过。未安装任何驱动，也未改变
  本机显示拓扑。
- 未包含：驱动包、提权安装/卸载（PLAT-01b）、启用/停用与 worker 接线
  （PLAT-01c）。

**PLAT-01c 策略层已完成（2026-11-03）**

- `include/chast/virtual_display.h` 新增 `VirtualDisplayPolicy`（从
  `host_settings.h` 移入）、`VirtualDisplayAction`、`VirtualDisplayPlan`、
  `VirtualDisplayPrepareResult`、`plan_virtual_display()` 与
  `prepare_virtual_display()`。规划是纯函数：`off` 不动作；`auto` 只在没有
  有面积的活动显示时动作；`on` 总是确保启用；不支持、冲突、未安装、扫描失败
  分别给出可读原因；数量钳制在 1～4；分辨率从 provider 的模式列表里按
  “精确 → 宽高比 → 面积”选择。
- `host/worker.cc` 在加载机器设置之后、创建 `HostServer` 之前调用
  `prepare_virtual_display()`，写入 `vdisplay:` 日志（状态、活动数量、失败
  原因）。未安装驱动、检测到冲突或 provider 动作未实现都不会阻止会话启动；
  `virtual_display_provider=vdd` 会明确记录“本版本未实现”。
- 托盘“安全设置 → 高级”页新增：策略下拉（关闭/无显示器时自动启用/总是
  启用）、数量输入（1～4）、分辨率下拉（usbmmidd 默认模式，保留当前自定义
  值）；保存写回 `host.ini`，中英文案同步。
- 单测：`tests/virtual_display_test.cc` 用假 provider 覆盖策略决策与编排
  （off/auto/on、有无活动显示、数量钳制、模式选择、provider 失败、启用后
  数量为 0），共 66 项通过；`host_settings`、`host_i18n` 同时通过。
- 全量回归：重新构建全部 Release 目标后 `ctest -C Release` 42/42 通过
  （639.55 s，2026-11-03 21:03），含此前因旧二进制误报的 `quic_loopback`、
  `quic_restart`、`quic_idle_disconnect`。
- 本切片仍未安装驱动、未改变本机显示拓扑。provider 的
  `set_monitor_count()` / `select_mode()` 仍是占位实现，因此 auto/on 在装有
  驱动的机器上会记录“not implemented in this build”；A2/A7/A8/A9 需要等
  PLAT-01b 的 deviceinstaller 与状态文件落地后做真机验收。

**PLAT-01b 代码脚手架已完成（2026-11-03，真机验收仍阻塞）**

- 驱动包契约：默认目录 `<exe>\drivers\usbmmidd_v2`；必需文件为
  `deviceinstaller64.exe`、`usbmmidd.inf`、`usbmmidd.cat`、
  `x64/usbmmidd.dll`、`Win32/usbmmidd.dll`、`License.txt` 和
  `manifest.sha256`。manifest 使用 sha256sum 风格并必须覆盖全部必需文件；
  逐文件 SHA-256 由 BCrypt 计算；`deviceinstaller64.exe` 通过
  WinVerifyTrust 校验 Authenticode；可选 `SOURCE.txt` 提供 `VERSION:` 与
  包级 `SHA256:`。`verify_virtual_display_package()` 返回
  Ok/Unsupported/Missing/HashMismatch/SignatureMismatch，驱动 catalog 仍交给
  PnP 在安装时验证。
- 状态文件：`%ProgramData%\chast\virtual-display.json`，字段为 provider、
  package_version、package_sha256、installed_at、owned、last_error；自带
  转义/解析，损坏时按无记录处理。`owned=false` 时卸载只复用、不删除驱动。
- CLI：`chast-host.exe --vdisplay install --package <目录> [--count 1-4]`、
  `uninstall`、`set-count <0-4>`、`status [--log <文件>]`；退出码
  0/2/3/4/5/6/7/8/9 与设计 §5.4 一致。`status` 无需提权；
  install/uninstall/set-count 检查提升令牌；服务路径永远不会自动安装或卸载。
- provider 动作已从占位实现替换为 deviceinstaller64 调用：安装后轮询 60 s，
  `enableidd 1/0` 每次增减一台并轮询 30 s，卸载先 `stop` 再 `remove`；
  `select_mode()` 用 `ChangeDisplaySettingsExW` 并先确认模式在枚举列表中。
  worker 使用 `default_virtual_display_package_dir()` 查找安装器。
- 验证：`chast-host`、`chast-service`、`chast-virtual-display-test`、
  `chast-host-i18n-test` 构建通过；`ctest -C Release -R
  "virtual_display|host_i18n|host_settings"` 3/3 通过；
  `chast-virtual-display-test` 共 106 项通过（新增 manifest/SHA-256/包校验/
  状态文件 JSON/CLI/时间戳/默认包目录）。实机
  `chast-host.exe --vdisplay status --log ...` 返回 0 并写入
  `state=not_installed provider=usbmmidd active=0 max=4`，未知动作返回 2。
  测试全部使用临时目录：一次早期运行曾因未知动作先于 `--log` 解析而把 usage
  写进真实 `%ProgramData%`，已改为优先提取 `--log` 并验证不再创建该目录。
  最终源码重新构建后全量 `ctest -C Release` 42/42 通过（640.11 s，
  2026-11-03），归属判定为“已存在的第三方驱动只复用、不认领”
  （`owned=false`），安装被拒或失败时保留原记录与安装时间。
- 仍未包含/阻塞：没有下载或提交驱动包，`tools/fetch-usbmmidd.ps1` 与第三方
  许可声明未创建，安装器/打包脚本未接 `--virtual-display`（PLAT-01d），
  A1～A10 均未执行。修复这两项前置条件后才能把 PLAT-01b 标记为完成。

**PLAT-01d 本地接线已完成（2026-11-04，真机验收仍阻塞）**

- 安装器：`chast-setup` 新增 `--virtual-display` 与
  `--virtual-display-count 1-4`，两者都要求 `--machine`；`checkElevation()`
  在提权检查前先做参数校验。`platformInstall()` 在解包负载之后、注册服务之前
  调用 `<根>\chast-host.exe --vdisplay install --package <根>\drivers\usbmmidd_v2
  --count N`，失败即终止安装并给出 `%ProgramData%\chast\vdisplay.log` 路径；
  驱动包缺失时明确提示用 `-VirtualDisplayPackageDir` 打包，不会自行下载。
- 卸载：仅 `--machine` 卸载会驱动 `--vdisplay uninstall`，且只在
  `%ProgramData%\chast\virtual-display.json` 存在时执行；卸载顺序是先驱动、
  后文件，驱动卸载失败会保留整个安装树以便重试。`owned=false` 的记录由 CLI
  复用语义保证不会被移除。
- 打包：`tools/package-windows.ps1` 新增 `-VirtualDisplayPackageDir`，省略时
  回退到仓库 `drivers\usbmmidd_v2`；包内必需文件齐全才复制到暂存树的
  `drivers\usbmmidd_v2\`，否则告警并继续生成不含驱动的安装包。
- 托盘：菜单新增“安装虚拟显示器…/卸载虚拟显示器…”入口。安装仅在
  `NotInstalled` 且默认包目录存在时可用，冲突驱动和缺失驱动包分别显示灰显
  说明；卸载仅在记录 `owned=true` 时可用，第三方安装只提示不认领。两个动作
  都先弹确认框，再以 `runas` 调起隐藏 CLI（安装 180 s、卸载 120 s 超时），
  失败对话框包含退出码、错误、日志最后一行和日志路径。`host/platform.cc`
  的提权逻辑抽成 `run_elevated()`，防火墙路径复用同一实现。
- 验证：全量 Release 构建（所有目标）退出码 0；`ctest -C Release -R
  "virtual_display|host_i18n|host_settings"` 3/3；`go test -count=1
  ./internal/setup` 通过（新增 5 个用例：选项校验、路径、缺 host/缺包拒绝、
  无记录跳过卸载、缺 host 时保留文件）；`package-windows.ps1` 解析通过；
  实机 `chast-host.exe --vdisplay status --log build/vdisplay-smoke.log`
  退出 0 且未创建 `%ProgramData%\chast`；`chast-setup --virtual-display`
  与 `--virtual-display-count 5` 均按预期在参数阶段被拒绝。
- 回归：同一构建的串行 ctest 分两段完成 42/42（1–24 于 2026-11-03、
  25–42 于 2026-11-04，18/18）。2026-11-04 重跑第一段时出现两次桌面像素类
  偶发：`quic_terminal` 一次、`quic_loopback` 的
  “captured picture was not the black overlay” 隐私亮度断言一次，两者单独
  重跑均通过；该断言比较运行期间桌面亮度变化，与本次托盘/安装器改动无关。
- 仍未包含/阻塞：没有下载、提交或安装驱动包；`tools/fetch-usbmmidd.ps1` 与
  `THIRD-PARTY-NOTICES.md` 占位见下一条，固定哈希、Amyuni 再分发确认、
  A1～A10 真机验收仍未完成。真机 `--virtual-display` 安装需要先提供固定
  哈希包。

**PLAT-01b 获取脚本与第三方声明占位（2026-11-04，固定哈希与许可仍阻塞）**

- `tools/fetch-usbmmidd.ps1`：fail-closed 获取脚本。`$pinnedZipSha256` 为空时
  只打印 `-SourceZip` 指向的本地包哈希并退出码 2，不发起下载；配置后从
  固定的 Amyuni URL 下载或读取 `-SourceZip`，校验包级 SHA-256，解包到
  `build/deps/usbmmidd_v2`，检查必需文件与唯一包根目录，生成 sha256sum
  风格的 `manifest.sha256` 和 `SOURCE.txt`（URL、日期、包哈希、INF
  `DriverVer`），并对 `deviceinstaller64.exe` 打印 Authenticode 状态与签名
  主体。目标目录已存在时先按 manifest 校验，通过则直接成功，否则要求
  `-Force`。退出码 2/3/4 分别表示未配置固定哈希、包哈希不符、包内容不完整。
- `THIRD-PARTY-NOTICES.md`：仓库根目录的第三方声明占位，记录 Amyuni 版权、
  产品页、版本 2.0.0.1、2026-11-03 复核的包 SHA-256、WHQL 签名链、不提交
  二进制、默认不捆绑、再分发与广告提示条款待确认、分发时保留 `License.txt`
  与 `SOURCE.txt` 等约束；许可全文/官方链接待 Amyuni 确认后补入。
- 验证：PowerShell 解析通过；无固定哈希直接运行退出码 2，未发起网络请求、
  未创建 `build/deps/usbmmidd_v2`。用临时合成包（占位文件 + 伪造 INF
  `DriverVer`）在 `build/` 下验证完整路径：生成 6 条 sha256sum 清单与
  `SOURCE.txt`（`VERSION: 2.0.0.1`）、重复运行直接判定“已存在且校验通过”、
  篡改文件后退出码 4、`-Force` 重新解包恢复、包哈希不符退出码 3 且不创建
  目标目录、解包临时目录被清理。`git diff --check` 干净。未下载、解包或
  安装任何真实驱动。
- 仍未完成：固定哈希（需人工复核官方包）、Amyuni 再分发/广告条款确认、
  许可全文、`drivers\usbmmidd_v2` 的实际打包与 A1～A10 真机验收。

**PLAT-01b ACL 与 TOCTOU 加固已完成（2026-11-04，真机验收仍阻塞）**

- 包信任与锁定：`verify_virtual_display_package()` 用
  `GENERIC_READ|READ_CONTROL` + `FILE_SHARE_READ` 打开包目录、manifest、
  全部 manifest 条目和可选 `SOURCE.txt`，拒绝 reparse point、错误对象类型
  与不可信 ACL；SHA-256 从已锁定的句柄读取，Authenticode 也在锁定期间校验。
  新增 `VirtualDisplayPackageGuard`，在 `deviceinstaller64.exe` 运行期间保持
  这些句柄；守卫存活时写入、删除、改名文件或改名包目录都会失败，释放后才
  恢复。
- ACL 信任配置：`State`（owner 与可写主体只允许 SYSTEM/Administrators，
  Users 只读）、`StateMigration`（额外允许当前提权用户和 CREATOR OWNER，
  后者会解析为已校验的 owner，用于给旧记录重新加 ACL）、`Package`
  （SYSTEM/Administrators/TrustedInstaller/当前用户，CREATOR OWNER 作为写
  主体）；任何允许其他 SID 写入的 allow ACE、以及带写位的 object/callback
  ACE 都判为不可信。
- 状态目录：`harden_virtual_display_state_directory()` 创建目录并写入
  `D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GR;;;BU)`，设置保护 DACL，
  启用 `SeRestorePrivilege` 把 owner 改为 BUILTIN\Administrators；不可信的
  旧 `virtual-display.json`/`vdisplay.log` 直接删除，可信旧文件重新加 ACL。
  install/uninstall/set-count 在动作前调用；`status` 仍无需提权，也不会
  创建 `%ProgramData%\chast`。
- 记录写入：`write_install_record()` 在可信目录内用 `BCryptGenRandom` 生成
  `.tmp-<hex>` 名、`CREATE_NEW` 打开、写入并 `FlushFileBuffers`、给临时文件
  加 `D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;BU)`，再用
  `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)` 原子替换，最后按 `State`
  配置读回校验。读取侧 `read_install_record()`、`virtual_display_log_tail()`
  和 `read_state_text()` 先验证目录/文件 ACL，不可信时按“无记录/无日志”
  处理；`append_log()` 只创建显式 `--log` 的自定义父目录，日志文件拒绝
  reparse point 和目录。
- 测试：`chast-virtual-display-test` 增至 130 项，新增包守卫（锁定期间写入/
  删除失败、`reset()` 后恢复）、目录 reparse 拒绝、状态目录信任、记录写入/
  读回/原子替换、无 `.tmp-` 残留、Users 可写记录被丢弃、带 CREATOR
  OWNER 写 ACE 的旧记录经迁移后保留、裸 ProgramData 目录既不信任记录也
  拒绝写入。签名用例改用带内嵌 Authenticode 签名的系统
  二进制（`MpSigStub.exe`/`appverif.exe`/`vsjitdebugger.exe`/ShowDesktop，
  按顺序取第一个可验证者）；catalog 签名的 `cmd.exe` 会被 WinVerifyTrust
  `WTD_CHOICE_FILE` 拒绝，不再使用。
- 验证：全量 Release 构建（所有目标）退出码 0；最终源码全量 `ctest -C
  Release` 42/42 通过（621.55 s，2026-11-04），定向
  `virtual_display|host_i18n|host_settings` 3/3；把 `ProgramData` 重定向到
  `build/programdata-smoke` 后，`--vdisplay status` 退出 0 且只写显式日志
  文件，`--vdisplay set-count 0` 先把目录加硬（`icacls` 显示 SYSTEM/
  Administrators 完全控制、Users 只读继承）再因驱动未安装退出 5，未生成
  记录文件；真实 `%ProgramData%\chast` 未被创建。`git diff --check` 干净。
- 已知边界：如果非管理账户把 `%ProgramData%\chast` 预先创建为 reparse
  point，或用拒绝管理员的 DACL 保护该目录，加固会失败关闭（不跟随链接），
  安装/卸载被拒绝，需要管理员手动处理；普通目录会被接管并重新加 ACL。
  这是可用性影响，不构成提权。旧的、继承 CREATOR OWNER 的记录只允许在
  迁移时被识别一次，迁移后立即替换为受保护 DACL。
- 仍未完成：固定哈希驱动包、Amyuni 再分发许可、许可全文、A1～A10 真机
  验收。本次加固不改变这些外部阻塞。

**PLAT-01 验收前差距修复与手册（2026-11-04）**

- A5：`--vdisplay install` 在平台检查后先探测冲突驱动；检测到 VDD/Parsec/
  RustDesk 标记时记录 `install refused: another virtual display driver is
  present: <id>` 并退出 8。托盘原本已灰显安装项，本次让安装器和直接 CLI
  调用同样不叠加驱动。
- A6：托盘菜单在 `HostServer` 处于 `Streaming` 时把卸载项改为灰显的
  “卸载虚拟显示器（请先结束会话）”，处理函数再次检查并只弹提示；结束会话后
  恢复可卸载。服务安装器的卸载会先停服务；直接 CLI 卸载仍不感知会话，
  见手册 G3。
- 新增实机验收手册 `docs/plat-01-acceptance-runbook-2026-11-04.md`，覆盖
  A1–A10 的前置、命令、期望输出、证据路径、回滚与已知差距。
- 验证：Release 构建 `chast-host` 与 `chast-virtual-display-test` 通过；
  定向 ctest `virtual_display|host_i18n|host_settings` 3/3；虚拟显示单测
  仍为 130 项通过。未安装驱动、未改变本机拓扑，A1–A10 仍未执行。

## 10. 风险与未决问题

2026-11-05 补充：已接入可选虚拟屏隐私、独立恢复进程、托盘启停及恢复后重新挂接路径，见 `virtual-display-privacy-2026-11-05.md`。下列驱动包、许可与真机验收要求仍然存在；此前“只有黑色遮罩”的描述以新文档为准。

1. **Amyuni 许可**：再分发是否允许、广告提示条款的实际表现需要法务/厂商确认；
   不允许时退化为引导下载。这是 PLAT-01b 的前置条件。
2. **2021 年驱动的兼容性**：Windows 11 24H2/25H2 可能改变显示驱动策略；
   A3 必须实测，失败则考虑 VDD 增强路径或自研 WHQL。
3. **服务/会话边界**：`deviceinstaller64 enableidd` 必须由用户会话中的 worker
   执行；需要验证 LocalSystem 提升后的子进程在目标会话中的显示枚举行为。
4. **多虚拟屏模式**：v1 只对第一台应用首选分辨率，其余用默认模式；
   若产品需要每屏独立分辨率，需要 VDD provider 或自研驱动。
5. **显示排列**：扩展屏放在右侧可能改变用户窗口位置；v1 只在会话启动前执行，
   并记录原拓扑，失败时回滚。是否需要恢复原排列待实测后决定。
6. **查看器侧控制**：后续协议版本可增加“列出/设置虚拟显示器”的消息；
   当前只做主机本地配置，避免协议和权限面扩大。
7. **冲突驱动**：RustDesk、Parsec、VDD 可能同时安装；检测到即拒绝自动安装，
   只提示用户手动选择，避免多个 IddCx 驱动争抢显示拓扑。

## 11. 参考

- `docs/rustdesk-gap-tasks-2026-11-01.md` PLAT-01
- Amyuni usbmmidd_v2：<https://www.amyuni.com/en/desktop-editions/usb-mobile-monitor/learn-more>
- RustDesk 改用 usbmmidd_v2：<https://github.com/rustdesk/rustdesk/discussions/6444>
- RustDesk 虚拟显示管理实现：<https://github.com/rustdesk/rustdesk/blob/master/src/virtual_display_manager.rs>
- Virtual-Display-Driver：<https://github.com/VirtualDrivers/Virtual-Display-Driver>
- VDD ARM64 签名问题：<https://github.com/VirtualDrivers/Virtual-Display-Driver/issues/465>
- Microsoft 驱动签名选项：<https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings>
