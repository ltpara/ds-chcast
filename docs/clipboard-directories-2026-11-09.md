# 文件剪贴板保留空目录

日期：2026-11-09。接续剪贴板方向控制，补齐复制文件夹时丢失空目录的问题。参考基线仍为本地 RustDesk `D:/rustdesk-ref` 的 `7423dced37845c48093b6f71bd82e4a16cc8c79d`；本文记录本项目实现和验证范围。

## 用户行为

主机到观看端、观看端到主机的文件剪贴板现在都保留独立空目录和文件夹中的嵌套空目录。只复制空目录也能完成同步，无需占位文件或文件数据流。零字节文件仍按普通文件收发，不会被当成目录。

单向剪贴板策略、按连接权限、取消与撤权继续生效。文件剪贴板仍独立于普通文件浏览/上传下载权限。主机的续传工作目录 `.chast-partial` 已移到会话暂存根目录，避免混入复制的目录树，影响粘贴内容和剪贴板回送判断。

## 实现

协议升级为 **v58**，主机和观看端需要一起更新。`ClipboardFileEntry` 新增 `directory`；FileList 标志位 bit1 表示目录，目录大小必须为零。目录类型参与剪贴板身份计算，零字节文件与空目录不再产生相同身份。编解码共同校验文件名与相对路径末段一致，拒绝未知标志和超限列表。

目录采集保留普通文件和空目录叶节点，由相对路径还原非空父目录。递归遍历保留深度和条目数限制，跳过重解析点（含 Windows junction），读取或枚举失败时拒绝整个列表，避免静默生成不完整副本。

双方收到目录元数据后，通过现有路径校验和逐级建目录逻辑创建空目录，再跳过这些条目的文件传输；列表中的原始索引保持不变。仅含目录的列表直接进入剪贴板写入阶段。暂存根目录使用新建目录，失败路径清理已创建的内容。目录创建沿用现有重解析点检查模型，不表示消除了所有文件系统并发替换风险。

## 验证

复用现有测试和 `chast-viewer-probe`，未新增 CMake 测试项目。

- `protocol`、`protocol_fuzz`、`file_transfer` 三项通过，共 2.55 秒，覆盖目录编码、文件/目录身份、非法元数据、真实目录采集、目录创建及重解析点拒绝；日志 `build/clipboard-directories-unit.log`。此后唯一产品代码修改是主机接收暂存布局，这三项的被测代码未再变化。
- 实际 ViewerEngine 与主机通过本机认证 QUIC 完成上下行混合场景：4 个顶层项、5 个普通文件，包含 Unicode 名称、零字节文件、独立空目录及嵌套空目录；核对字节、目录结构和重读后的剪贴板身份。日志 `build/clipboard-directories-down.log`、`build/clipboard-directories-up.log`。
- 同样完成上下行纯目录场景：2 个顶层目录、4 个目录叶节点，没有文件数据传输。日志 `build/clipboard-directories-empty-down.log`、`build/clipboard-directories-empty-up.log`。上述四个场景均使用对应单向剪贴板策略，并关闭普通文件权限。上行两项使用最后一次暂存布局修复后的程序；下行两项不经过该修改路径。
- 最后一次主机暂存布局修复后，完整 `host_parity` 与 `clipboard_async` 回归均通过，分别为 104.39 秒和 16.20 秒，共 120.61 秒；日志 `build/clipboard-directories-session-tests.log`。
- Release 主机、服务、现有验证程序及 Windows 观看端构建通过。日志 `build/clipboard-directories-build.log`、`build/clipboard-directories-final-build.log`、`build/clipboard-directories-viewer-final-build.log`。仍有原有编译/链接警告。
- 观看端运行资源已安装，见 `build/clipboard-directories-viewer-bundle.log`。程序时间为主机 02:59:08、服务 02:59:14、观看端 02:56:58（本地 2026-11-09）。最后的主机接收路径修复不涉及观看端编译内容。
- 作用范围内的差异空白检查通过；Windows CRLF 文件使用 `git -c core.whitespace=cr-at-eol diff --check`，不因换行格式重写文件。

## 边界

文件剪贴板仍在复制后预传并暂存，粘贴时按需读取尚未实现。现有 256 项、64 KiB 列表、目录深度及文件大小限制继续保留。同名顶层目录的歧义没有重新设计。

本轮端到端验证采用真实会话及 ViewerEngine，但注入剪贴板读取/写入回调，没有修改用户实际剪贴板；尚未验证 Explorer 实际粘贴、跨网络使用或多用户/RDP 切换。更广泛的发射端开发目标仍未完成。
