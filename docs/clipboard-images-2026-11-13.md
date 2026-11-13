# 剪贴板 PNG 与大图像（协议 v75，2026-11-13）

## 1. 结论

协议 **v75** 和 CCLB **v4** 为剪贴板增加独立 PNG 表示，解决大截图和
`CF_DIB` 超过传输上限时无法复制的问题。PNG 编解码、严格格式校验、Windows
格式注册、双向 CCLB/QUIC 传输以及损坏 PNG 的独立回退均已实现并通过本机
自动化。

当前证据等级是“代码完成、单机自动化与原生私有窗口站通过”。工作区直接运行
CTest 时仍继承 Low 完整性标签，不能创建私有窗口站；把探针复制到用户临时
目录并显式设为 Medium 完整性后，原生 PNG、`image/png` 独立句柄、大 DIB 转
PNG 和坏 PNG 的 DIB 回退均实际通过。真实双机 Explorer/Office 粘贴和弱网场景
仍待验收。

## 2. 协议与内容模型

`ClipboardFormat` 在 `FileList = 4` 之后增量增加 `Png = 5`，既有编号保持不变。
CCLB 从 v3 升到 v4；每条目 zstd 压缩、stored/raw 双长度和总哈希校验继续沿用。
Hello 的协议版本门禁保证旧 CCLB 版本不会与 v75 会话混用。

PNG 是整个 PNG 文件的字节，而不是仅 IHDR/IDAT 载荷。一个剪贴板项可以同时携带
Text、HTML、RTF、DIB 和 PNG；接收端在一次剪贴板所有权变化中写回所有可用格式，
让粘贴目标自行选择最合适的一种。

图片格式是替代表示，不是同一个逻辑项只能二选一：

- 源应用提供有效 PNG 时直接转发，并可在 DIB 未超限时同时保留 DIB；
- 只有 DIB 且 DIB 超过网络上限时，本地转换为 PNG，网络只携带 PNG；
- PNG 损坏时仍可接受同一项中的有效 DIB，不因图片格式之一失败而连带丢弃文本、
  HTML 或 RTF；
- 任一格式的读取、解码或容器校验失败仍会使该次基线读取失败，避免半成品被当作
  新复制内容。

## 3. 限制与校验

| 项目 | 限制 |
| --- | --- |
| 单项 Text | 4 MiB |
| 单项 HTML / RTF | 各 4 MiB |
| 网络 DIB | 32 MiB |
| PNG | 64 MiB |
| 剪贴板总流 | 112 MiB |
| PNG 解码尺寸 | 单边不超过 16384，像素数不超过 64 Mi |
| 本地 DIB 转换源 | 最多 160 MiB，只在发送端读取阶段使用 |

网络 DIB 上限仍为 32 MiB；不能通过提高 DIB 上限替代 PNG 路径。160 MiB 仅覆盖
发送端把本地大 DIB 转成 PNG 的临时输入，该 DIB 本身不会进入 CCLB。

PNG 在进入内容模型前检查签名、完整 chunk 边界、CRC、IHDR 的合法位深/颜色类型、
压缩/过滤字段、`IDAT` 与 `IEND` 顺序、尾随字节以及尺寸/像素预算。WIC 负责
PNG 编解码，`chast.png` 是独立静态库，剪贴板测试不依赖 D3D11 采集栈。无压缩
24/32 位 DIB 由转换器直接按行读取并翻转 bottom-up 行，避免依赖 WIC 对超大
top-down BMP 的解码；位域、RLE 和嵌入 JPEG/PNG 仍交给 WIC。

## 4. Windows 剪贴板行为

- 读取同时识别非正式名称 `PNG` 与 MIME 名称 `image/png`，优先使用能通过完整
  校验的表示；
- 写入把 PNG 同时发布到 `PNG` 和 `image/png`，每个格式使用独立 `HGLOBAL`；
- 有效大 DIB 在本地转换后仍受 PNG 64 MiB 和像素预算约束；
- 写入先准备所有格式，再清空剪贴板；单个格式被系统拒绝不会留下半构造内容；
- 损坏 PNG 和有效 DIB 共存时，读取结果保留 DIB，并尝试从 DIB 生成 PNG。

`chast-viewer-probe --clipboard-native` 使用私有窗口站和桌面，验证上述契约，
包括两个 PNG 格式句柄互不共享、33 MiB 级 DIB 只以 PNG 读取，以及坏 PNG 的 DIB
回退。该测试不会读取、保存或替换交互用户剪贴板。CTest 注册名为
`viewer_clipboard_native`，超时 60 秒，返回 77 表示当前 token 无权限创建私有
窗口站。Low 完整性工作区会返回 77；Medium 完整性临时副本已实际运行并返回 0。

## 5. 自动化证据

2026-11-13 在本机 Release 构建执行的定向回归：

```powershell
ctest --test-dir build/root -C Release -R '^(png_writer|protocol|clipboard_async|protocol_fuzz)$' --output-on-failure
```

结果为 4/4 通过：

- `png_writer`：PNG 文件/内存编解码、WIC 回读、DIB 转换和参数边界；
- `protocol`：v75 PNG 编号、严格 PNG 校验、CCLB v4 往返与超限拒绝；
- `protocol_fuzz`：协议解析变异回归；
- `clipboard_async`：真实 ViewerEngine、HostServer 和 QUIC 回环双向传输完整
  PNG 字节，host→viewer 与 viewer→host 均有独立断言。

同一构建随后执行宽范围剪贴板回归：

```powershell
ctest --test-dir build/root -C Release -R '^(clipboard_async|clipboard_listener|clipboard_listener_startup|clipboard_oledata|viewer_clipboard_files|viewer_clipboard_files_up|viewer_clipboard_native)$' --output-on-failure
```

CTest 直接运行汇总 7/7 成功，但不能把 3 个明确 `Skipped` 的条目记成通过：

- 实际通过 4 项：`clipboard_async`、`clipboard_oledata`、
  `viewer_clipboard_files`、`viewer_clipboard_files_up`；
- `clipboard_listener`、`clipboard_listener_startup` 和
  `viewer_clipboard_native` 因当前执行 token 不能创建私有窗口站而跳过。

全部 `viewer_` 场景另执行：

```powershell
ctest --test-dir build/root -C Release -R '^viewer_' --output-on-failure
```

CTest 汇总 9/9 成功，其中 8 项实际通过、`viewer_clipboard_native` 明确跳过。
同一构建的 `chast-viewer-probe --multi-shares` 也通过真实 QUIC 多共享根、
递归下载、上传和管理操作；探针接收目录位于唯一的
`build/root/test-temp/chast-multi-share-probe-*/downloads`。

上述 Viewer 探针通过测试专用接收目录覆盖接口运行，不读取、写入或移动真实
`Downloads\chast`。覆盖接口只允许在 ViewerEngine 无会话时设置为绝对路径；
空值仍恢复生产默认目录。`%TEMP%` 不可写的受限环境使用构建树内的
`test-temp`。

完整 `chast-quic verify` 同样通过。host→viewer 的 CCLB 实测携带 Text、HTML、
RTF、DIB、PNG 五种格式，PNG 字节校验通过；viewer→host 超限 CCLB 被拒绝后，
连接和后续剪贴板更新继续工作；两端 `clipboard errors=0`。

直接从 Low 完整性工作区运行原生项时仍返回 `ERROR_ACCESS_DENIED`，所以 CTest
结果明确为 `Skipped`：

```text
viewer_clipboard_native ..........***Skipped
skipped: the process token cannot create a private clipboard station
```

该跳过只反映工作区标签，不能作为原生测试失败或通过的替代证据。把同一
Release 构建的三个原生程序复制到唯一临时目录、设为 Medium 完整性后执行：

```powershell
$dest = Join-Path $env:TEMP ('chast-clipboard-native-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $dest | Out-Null
Copy-Item build/root/Release/chast-viewer-probe.exe,
          build/root/Release/chast-clipboard-listener-test.exe,
          build/root/Release/chast-clipboard-listener-startup-test.exe,
          build/root/Release/msquic.dll -Destination $dest
icacls.exe $dest /setintegritylevel Medium /T | Out-Null
Push-Location $dest
.\chast-viewer-probe.exe --clipboard-native
.\chast-clipboard-listener-test.exe
.\chast-clipboard-listener-startup-test.exe
Pop-Location
```

三个程序的实际退出码均为 0。PNG 探针输出两组通过信息：

```text
private native clipboard: independent PNG and image/png handles,
oversized DIB to PNG, and broken-PNG DIB fallback passed
private OLE clipboard: metadata-only publish, marshalled IStream read
and ownership cancellation passed
```

监听器测试覆盖双订阅、代际推进、退订隔离和最后订阅者停止监听；启动测试覆盖
消息窗口创建超时、失败后的待重试状态和消息循环丢失后的恢复。

```text
ok   both subscribers hear the change
ok   generation advances
ok   unsubscribed session is not called
ok   last unsubscribe stops the listener
clipboard listener checks: 0 failure(s)
clipboard startup timeout, pending retry and lost-loop recovery passed
```

因此工作区 Low 标签下 CTest 的计数仍是 4/4 实际通过、3 项跳过；补做 Medium
完整性临时副本后，宽范围涉及的 7 项均已有实际通过证据，全部 Viewer 场景的
9 项也均已有实际通过证据。直接 CTest 的 `Skipped` 与补做证据应分别记录。

测试仍使用私有窗口站/桌面，不读取或替换当前用户的真实剪贴板。

## 6. 剩余验收

1. 使用两台机器验证 host→viewer 和 viewer→host 双方向的大截图、透明 PNG、
   多格式混合项，以及 Explorer、Office、浏览器和聊天软件的实际粘贴。
2. 在弱网、断线和重连期间验证 64 MiB PNG、112 MiB 总流以及超限拒绝不破坏会话。
3. 验收前保持本项为“单机自动化与原生私有窗口站通过、双机待验收”，不能外推为
   全平台完成。

## 7. 复现入口

```powershell
cmake --build build/root --config Release --target chast-png-writer-test chast-protocol-test chast-clipboard-async-test chast-protocol-fuzz-test chast-quic chast-viewer-probe --parallel 4
ctest --test-dir build/root -C Release -R '^(png_writer|protocol|clipboard_async|protocol_fuzz)$' --output-on-failure
ctest --test-dir build/root -C Release -R '^(clipboard_async|clipboard_listener|clipboard_listener_startup|clipboard_oledata|viewer_clipboard_files|viewer_clipboard_files_up|viewer_clipboard_native)$' --output-on-failure
ctest --test-dir build/root -C Release -R '^viewer_' --output-on-failure
ctest --test-dir build/root -C Release -R '^viewer_clipboard_native$' --output-on-failure
Push-Location build/root
.\Release\chast-viewer-probe.exe --multi-shares
.\Release\chast-quic.exe verify
Pop-Location
```
