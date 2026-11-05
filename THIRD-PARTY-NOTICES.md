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

- 仓库不提交第三方二进制。`tools/fetch-usbmmidd.ps1` 已固定上述 SHA-256
  （2026-11-06 复下载一致），下载并校验安装包后解包到
  `build/deps/usbmmidd_v2`（`build/` 已被 `.gitignore` 忽略），并生成
  `manifest.sha256` 与 `SOURCE.txt`。
- 再分发许可仍未确认：该固定哈希目前只用于内部验收测试，发布包仍不得包含
  驱动二进制。
- `tools/package-windows.ps1 -VirtualDisplayPackageDir <目录>` 或仓库
  `drivers\usbmmidd_v2` 存在时才把驱动打进安装包；默认安装包不含该驱动。
- 再分发许可与广告提示条款尚未获得 Amyuni 确认。确认前发布包不应包含该
  驱动二进制，只保留“用户主动获取 + 哈希校验 + 明确确认”的引导安装。
- 分发该驱动时必须原样保留 `License.txt`，并在安装目录附带 `SOURCE.txt`
  （下载 URL、日期、SHA-256）。
- 待办：Amyuni 确认后再在本文件追加许可全文或官方许可链接。
