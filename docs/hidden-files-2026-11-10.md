# 隐藏文件浏览选项

日期：2026-11-10。接续 [发射端模块对比](host-rustdesk-module-comparison-2026-11-08.md) 的 G18/C31 缺口；RustDesk 对照仍为本地固定提交 `7423dced37845c48093b6f71bd82e4a16cc8c79d`。RustDesk 在 `libs/base/src/fs.rs:53` 的 `read_dir(path, include_hidden)` 对文件和目录统一跳过 `FILE_ATTRIBUTE_HIDDEN (0x2)`，递归下载在 `libs/base/src/fs.rs:233` 通过 `get_recursive_files(path, include_hidden)` 沿用同一开关，开关本身是连接级 FileTransfer 选项（`src/client.rs:3878`、`src/server/connection.rs:2786`）。本轮按此行为补齐隐藏项过滤，不改变共享根、多盘符和压缩块传输。

## 用户行为

文件面板工具栏新增“显示隐藏文件”眼睛按钮，开启时按钮高亮；默认关闭，与 RustDesk 默认 `show_hidden=false` 一致。切换时清空当前选择并重新请求当前目录第一页，分页游标始终按当前视图生成，因此隐藏项不会挤进“加载更多”的偏移。

递归下载（批量下载和整文件夹下载）沿用点击时文件面板的隐藏视图：关闭隐藏项时跳过隐藏文件和隐藏目录，开启时一并打包。这对应 RustDesk `get_recursive_files(include_hidden)` 的语义，而不是固定只下载可见列表。

选择是每个设备的视图偏好（`browser_show_hidden`），重开会话或切换显示器子窗口后沿用；与文件排序、起始目录等共享字段互不覆盖。

## 协议和主机

协议升级到 **v65**，两端必须同版本部署。`FileListRequest` 在原有 `request_id + directory + cursor` 之后增加一个 `flags` 字节（见 `include/chast/protocol.h` 的 `kFileListFlagShowHidden = 0x01`）。bit0 置位表示连隐藏项一起列出；其他位保留，出现未知位时解析失败。旧版本主机不会读取该字节，会把带 flags 的请求判为尾随字节错误，因此不能只升级一端。

主机仍用 `FindFirstFileW` 枚举共享根内的相对目录，在游标和分页处理之前跳过 `FILE_ATTRIBUTE_HIDDEN` 条目（文件与目录同一规则），同时继续跳过 `.`、`..` 和 reparse point。过滤发生在分页前，保证“加载更多”的顺序和数量与当前视图一致；隐藏项不会在第二页才出现或被重复计数。路径解析本身不按隐藏属性拒绝，因此进入一个隐藏目录后仍可正常列出其内容——与 RustDesk 的文件树行为一致，隐藏项是显示过滤而非安全边界。

观看端递归下载的每一页目录列表都携带同一 `show_hidden` 值，避免父目录按一个视图遍历、子目录又换回另一个视图。

## 观看端

原生引擎的 `RequestFileList`、`RequestBatchDownload`、`RequestDirectoryDownload` 和下载队列都增加 `show_hidden` 参数，`FileListAnswer` 回显请求时的值；桥接 map 增加 `showHidden`，Dart 侧 `requestFileList`、`downloadFolder`、`downloadEntries` 增加同名可选参数。文件面板把回显值和当前请求视图比对，旧视图迟到的回答不会覆盖新列表。

`ViewPreferences` 新增 `browser_show_hidden`（默认 `false`），会话页按设备身份读取并在切换时写回；主会话和显示器子窗口共用该值。英文界面新增“Show hidden files / Hide hidden files”。

## 验证

- 协议定向测试通过：`chast-protocol-test` 覆盖带游标的 `show_hidden=true` 往返、默认 `false` 以及未知 flags 位拒绝；`chast-protocol-fuzz-test` 的 79 个解析入口全部通过。
- 文件传输定向测试通过：`chast-file-transfer-test` 在共享根创建隐藏文件和隐藏目录，默认列表仍只返回 3 个普通条目，`show_hidden=true` 返回 5 个且目录优先、隐藏文件排在末尾并带正确大小。
- `flutter analyze --no-pub` 无问题；全量 `flutter test` 701/701 通过，其中文件面板 51 项包含“隐藏项开关重新加载并忽略另一视图”的迟到回答，视图偏好 16 项覆盖默认值与 `browser_show_hidden` JSON 往返。
- Windows 观看端 Release 构建通过：`flutter build windows --release`（249 秒），产物为 `app/build/windows/x64/runner/Release/chast_viewer.exe`。
- 真实 QUIC 回环通过：`chast-quic verify --port 45510 --file-browse --quiet`（114 项检查全部通过），新增断言“the hidden file appeared only when the viewer asked for hidden entries”确认默认列表不含隐藏文件、显式视图包含它；同一回环继续覆盖分页、越界拒绝、下载、上传、创建/重命名/递归删除。
- 完整 Release `ctest` 结果见下方“全量回归”一节。

## 全量回归

本切片提交时完整 Release `ctest` 仍在本机后台运行（日志只在 `build/`，不进入仓库），提交内容不声称全量回归通过；上文列出的定向测试、构建和真实 QUIC 回环均已实际执行。完整结果若与后续记录不符，以新的模块文档为准。

## 边界

- 只识别 Windows `FILE_ATTRIBUTE_HIDDEN`；不把 `.` 开头的文件名当作隐藏约定，也没有独立的“系统文件”开关（RustDesk 同样只过滤 HIDDEN 位）。
- 隐藏项只是列表过滤：显式路径、上传、下载、删除和递归下载在开启视图后都可操作隐藏项；不宣称隐藏属性提供保护。
- 仍只有单一共享接收根；多共享根/盘符和可协商压缩块属于 G18 后续分片。
- 万条隐藏项的目录与普通目录共用 1024 条分页和 200000 项遍历上限；未对超大隐藏树单独压测。
