# 第三方声明与许可

本文件记录随 chast 分发或由 chast 工具获取的第三方组件。其他第三方组件
（包括 MsQuic、Opus、Flutter 及其依赖）按各自发行树中的许可文件处理。

## Amyuni usbmmidd_v2（可选虚拟显示驱动）

- 组件：USB Mobile Monitor Virtual Display（`usbmmidd_v2`）
- 版权：Amyuni Technologies（具体版权声明以随包 `License.txt` 为准）
- 产品页：<https://www.amyuni.com/en/desktop-editions/usb-mobile-monitor/learn-more>
- 许可：随包提供的 `License.txt`；本仓库当前不包含许可全文
- 版本：2.0.0.1（INF `DriverVer = 08/31/2021,2.0.0.1`）
- 来源：<https://www.amyuni.com/downloads/usbmmidd_v2.zip>
- 2026-11-03 复核的安装包：199,309 字节，SHA-256
  `629b51e9944762bae73948171c65d09a79595cf4c771a82ebc003fbba5b24f51`
- 签名链：Microsoft Root Certificate Authority 2010 → Microsoft Windows
  Third Party Component CA 2014 → Microsoft Windows Hardware Compatibility
  Publisher，时间戳 2021-09-09（WHQL）；`signtool verify /pa /v` 覆盖
  `usbmmIdd.cat` 与 `x64/usbmmIdd.dll`
- 逐文件哈希与选型依据：`docs/plat-01-virtual-display-2026-11-03.md` §3.1

### 分发状态

- 仓库不提交第三方二进制。`tools/fetch-usbmmidd.ps1` 在 `$pinnedZipSha256`
  配置后下载并校验安装包，解包到 `build/deps/usbmmidd_v2`（`build/` 已被
  `.gitignore` 忽略），并生成 `manifest.sha256` 与 `SOURCE.txt`。
- 2026-11-05 再次从官方地址获取相同 ZIP，已固定 ZIP 及必需文件的 SHA-256，
  允许主动获取用于本机安装和验证。此操作不代表批准发布包再分发。
- 官方 `deviceinstaller64.exe` 没有 Authenticode 签名。主机仅对完整匹配内置
  六个文件哈希的 2.0.0.1 包允许这个辅助程序，同时仍验证 CAT、x64/Win32 DLL
  的 Authenticode、文件 ACL 与只读锁。修改 manifest 或 SOURCE 不能授权新文件。
- `tools/package-windows.ps1 -VirtualDisplayPackageDir <目录>` 或仓库
  `drivers\usbmmidd_v2` 存在时才把驱动打进安装包；默认安装包不含该驱动。
- 再分发许可与广告提示条款尚未获得 Amyuni 确认。确认前发布包不应包含该
  驱动二进制，只保留“用户主动获取 + 哈希校验 + 明确确认”的引导安装。
- 分发该驱动时必须原样保留 `License.txt`，并在安装目录附带 `SOURCE.txt`
  （下载 URL、日期、SHA-256）。
- 待办：发布包再分发前确认 Amyuni 许可及广告提示条款；固定哈希只用于验证
  已审阅文件，不替代这项确认。原许可随本机驱动包保留。
