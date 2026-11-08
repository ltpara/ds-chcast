# 剪贴板方向控制

日期：2026-11-09。接续本地 RustDesk 固定版本 `7423dced37845c48093b6f71bd82e4a16cc8c79d` 的模块差距清单，补齐本地主机的单向剪贴板策略。本文描述本项目实现，不表示两个项目的全部剪贴板语义一致。

## 用户行为

原生主机和服务机器设置的“安全设置 → 访问控制”新增“剪贴板方向”，支持双向、本机到查看端、查看端到本机、禁止同步。文本、HTML、RTF、图片和文件剪贴板共用这项策略。剪贴板总开关、按连接授权和查看端暂停同步开关仍是额外限制。

配置字段为 `clipboard_direction`：`both`（默认）、`from_host`、`to_host`、`disabled`。旧配置缺少此字段时保持双向；无效值会禁止同步并产生配置警告。方向只允许本地修改，不加入远程配置字段。保存沿用原生设置的重启共享/服务流程，当前连接会重建；本轮没有增加不中断连接的方向修改。

查看端工具栏剪贴板按钮的提示显示单向限制。被禁止发送的一端不调用本地剪贴板读取回调；准许发送时继续先建立基线，避免连接后立即发送历史内容。禁止接收的一端不会应用对端内容。

## 协议与权限

协议升级为 **v57**，发射端和查看端需要一起更新。`HelloAck`/`StreamInfo` 原有的第二字节改为主机视角的方向值：0 禁止、1 本机到查看端、2 查看端到本机、3 双向。该策略与剪贴板权限位独立保存，撤销后恢复权限或切换显示器不会扩大方向。

QUIC 在文本消息、富剪贴板流识别/完成、发送接口和文件路径上执行相应方向约束。主机会话另外检查接收和发送条件，覆盖文本、图片、复制文件列表、临时文件写入和 `.chast-clipboard/<index>` 下载。

本轮同时修复了文件剪贴板错误依赖普通文件传输权限的问题。复制文件的发送任务和 `FileOffer` 保留 `clipboard` 标记，双方按标记选择剪贴板或普通文件权限。传输层记录收到的 offer 及接受状态，CFIL 数据流必须匹配已接受的 offer；任意标记为剪贴板的上传仍需匹配主机当前收到的文件列表，查看端只接收自己请求的文件剪贴板 offer。

撤权按传输类别取消队列和数据流，普通文件与文件剪贴板不会借用另一类权限。发送数据、结束上传和接收数据块再次检查权限；共享策略的原子撤权即使早于主机会话清理，也不会继续缓存被禁止的块。已排队的旧文本更新也会在撤权时清除。

## 验证

- Release 主机、服务、观看端和既有验证程序构建通过；日志 `build/clipboard-direction-build.log`、`build/clipboard-direction-final-build.log`、`build/clipboard-direction-viewer-build.log`。仍存在原有编译/链接警告。
- 既有 `protocol`、`host_settings`、`host_i18n` 全部通过（0.48 秒）；`protocol_fuzz` 通过（0.75 秒）。覆盖协议方向、无效值、配置默认值及保存、机器设置合并。日志 `build/clipboard-direction-basic-tests.log`、`build/clipboard-direction-fuzz.log`。
- 既有 `host_parity` 的方向专项通过，使用真实认证 QUIC 和模拟媒体、剪贴板回调，覆盖四种方向、基线、禁止方向、独立的文件权限、字节完整性和撤权。日志 `build/clipboard-direction-focused.log`。
- 完整 `host_parity` 与 `clipboard_async` 回归通过，分别 103.74 秒、16.97 秒。日志 `build/clipboard-direction-session-tests.log`。没有新增 CMake 测试项目。
- 现有 `chast-viewer-probe --clipboard-files` 和 `--clipboard-files-up` 分别使用对应单向模式，并关闭普通文件权限；实际 ViewerEngine 与主机各完成 3 个顶层项、4 个文件的收发，包含 Unicode 文件名和嵌套目录，核对文件内容。接收模式下查看端未读取本地剪贴板，上行无回送、失败数为 0。日志 `build/clipboard-direction-viewer-down.log`、`build/clipboard-direction-viewer-up.log`。
- Flutter 既有剪贴板状态测试 6 项通过，定向 Dart 分析无问题。日志 `build/clipboard-direction-flutter.log`、`build/clipboard-direction-dart-analyze-final.log`。
- 复用离屏原生界面辅助程序，中英文加载/保存方向均通过，检查了控件位置和截图；见 `build/clipboard-direction-ui-en.log`、`build/clipboard-direction-ui-zh.log`、`build/clipboard-direction-settings-en.png`、`build/clipboard-direction-settings-zh.png`。

最终检查补充：数据块接收路径增加共享策略撤权的再次检查后，重建了主机、服务、会话验证程序及观看端，见 `build/clipboard-direction-revocation-build.log`、`build/clipboard-direction-viewer-final-build.log`。方向专项再次通过，新增“普通文件权限保持开放、正在传输的剪贴板文件被撤销”的场景，确认复制文件停止而目录浏览继续可用；见 `build/clipboard-direction-focused-final.log`。最终实际观看端上下行检查、Flutter 6 项状态测试均通过，见 `build/clipboard-direction-viewer-down-final.log`、`build/clipboard-direction-viewer-up-final.log`、`build/clipboard-direction-flutter-final.log`。这次小范围收尾使用专项验证，没有把前面的完整回归说成最后一次构建的完整复跑。

观看端运行资源已安装到 `app/build/windows/x64/runner/Release`，日志 `build/clipboard-direction-viewer-bundle.log`。最终程序时间：主机 02:44:10、服务 02:44:20、观看端 02:44:18（本地 2026-11-09）；源文件之后只有协议版本/接口说明注释整理。作用范围内的 `git diff --check` 通过。

## 边界

方向策略按剪贴板整体生效，尚未细分为只允许文本或单独限制图片/文件。文件剪贴板仍在复制后预传并暂存，粘贴时按需读取和空目录表达仍待开发。普通文件浏览、上传下载仍由文件权限控制，剪贴板方向不等价于阻止所有文件外发。测试采用本机环回连接和替代剪贴板回调，没有修改用户实际剪贴板、正式服务设置或验证跨网络使用效果。
