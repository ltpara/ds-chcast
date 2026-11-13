# 临时自由手写标注（G21 / v76）

## 1. 状态

2026-11-14 完成 G21 的“临时自由手写标注”源码实现、自动化测试、Release
构建、单机真实 QUIC 控制链路回环，以及真实 `HostServer` 单机可见覆盖层自动化
验收。标注只存在于当前主机会话，不写入磁盘，会话结束即销毁。

当前证据边界如下：

| 项目 | 状态 |
| --- | --- |
| 协议、查看端输入模型、主机覆盖层 | 已实现 |
| Flutter 与原生定向自动化 | 已通过 |
| Windows 查看端 Release 构建 | 已通过 |
| 单机真实 QUIC 控制链路 | 已通过 |
| 真实 `HostServer` 单机可见覆盖层（自动化） | 已通过 |
| 真实双机查看器 → 主机 | 未验收 |
| 真实多屏、混合 DPI、负坐标目视位置 | 未验收 |
| 生产发布与升级回退 | 未验收 |

GitHub `main` 的提交基线仍是协议 v73（`5590d75`）；本文记录的是尚未提交的
工作区协议 v76。v76 与 v75 及更早版本不承诺线协议互通。

## 2. 用户行为

1. 查看端会话工具栏提供“标注”开关，主会话和独立显示器窗口均可进入标注模式。
2. 标注模式下主指针拖动只生成主机桌面上的临时笔迹，不再经过远程鼠标、触摸或
   设备指针回调。标注不要求查看端拥有输入控制权限。
3. 点按会形成一个圆点；拖动产生连续线条；窗口失焦、指针取消或退出标注模式会
   结束当前笔画。
4. “撤销”删除主机上的最新一笔，“清除”删除当前会话的全部笔迹。标注不会保存到
   截图、录像或剪贴板，宿主桌面本身也不被修改。

## 3. 协议与传输

协议 v76 新增 `ControlType::Annotation = 89`，使用现有 QUIC 控制流的严格长度
前缀消息。动作包括：

| 动作 | 值 | 点数组语义 |
| --- | ---: | --- |
| `Clear` | 0 | 必须为空 |
| `Begin` | 1 | 一个起始点，`stroke_id` 非零 |
| `Move` | 2 | 1–64 个后续点 |
| `End` | 3 | 一个终点，关闭该笔画 |
| `Undo` | 4 | 必须为空 |

每个消息最多 64 点；坐标使用虚拟桌面像素，范围为
`[-1048576, 1048576]`，因此可以表达负原点和跨越多个输出。未知动作、零
`stroke_id` 的笔画动作、非法点数、越界坐标、尾随字节和空编码均被解析层拒绝。

主机侧使用有界 FIFO 保持消息顺序。查看端在发送层也保持顺序：`Begin` 立即发送，
`Move` 约 33 ms 或累计 64 点触发，`End` 强制冲刷余下点；发送失败不会阻塞后续
笔画。主窗口和独立显示器窗口共享全局笔画 ID 分配，避免两个窗口复用同一 ID。

## 4. 主机覆盖层

Windows 主机为每个显示器创建独立的分层窗口：

- `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`，
  点击穿透、不接收焦点、不出现在任务栏。
- 每个平面使用预乘 BGRA DIB，笔画由纯函数光栅模块绘制，支持负坐标、裁剪、圆端
  与半透明阴影。
- 主机按 `stroke_id` 分别维护活动状态。主会话和独立显示器窗口交错发送
  `Begin/Move/End` 时，各自的后续点仍接在正确笔画上；已结束或已被撤销的笔画
  不会因迟到的 `Move` 被重新创建。
- 历史上限为 2048 笔、65536 点，超过后淘汰最旧笔画；单个超长笔画也会只保留最新
  端点，内存不随会话时长无限增长。
- 覆盖层由对应 `HostServer` 会话持有并在断线、会话结束或析构时销毁。

## 5. 验证证据

原生 Release 构建：

```powershell
cmake --build build/root --config Release --target chast-protocol-test chast-annotation-paint-test chast-quic chast-host --parallel 4
```

通过。定向测试：

```powershell
ctest --test-dir build/root -R protocol -C Release --output-on-failure
ctest --test-dir build/root -R annotation_paint -C Release --output-on-failure
```

`protocol`、`protocol_fuzz` 与 `annotation_paint` 全部通过。标注光栅测试覆盖裁剪、
圆点、叠加、预乘 alpha；新增历史测试覆盖两个交错活动笔画、撤销最新一笔、已结束
笔画拒绝迟到点、按笔数淘汰和超长笔画截断。

真实 QUIC 回环：

```powershell
chast-quic.exe verify --port 45633 --seconds 2 --quiet --annotation-overlay
```

不启用覆盖层探针时为 104 项，启用后为 105 项，全部通过。基础回环使用输入权限
关闭的服务端，依次发送
`Begin/Move/End/Undo/Clear`，验证负坐标、笔画 ID、点数、动作顺序与队列内容。
`--annotation-overlay` 另经真实 `HostServer`、`QuicServer` 与 `QuicSession`
客户端建立会话，枚举当前进程的 `chast_annotation_overlay` 窗口并使每个显示器
恰好对应一个覆盖层；逐项核验窗口样式、`WM_NCHITTEST == HTTRANSPARENT`、
窗口矩形与显示器矩形一致、没有输入权限时仍可显示，以及 `Clear` 后隐藏。
探针还以 `BitBlt(SRCCOPY | CAPTUREBLT)` 截取屏幕局部并比较标注前后像素，要求
可见屏幕至少变化 1000 像素；会话结束后确认覆盖层窗口已销毁。

Flutter：

```powershell
flutter analyze --no-pub
flutter test --no-pub test/annotation_stroke_controller_test.dart test/remote_view_draw_test.dart
flutter build windows --release --no-pub
```

静态分析无问题，12/12 定向测试通过，Release 生成
`app/build/windows/x64/runner/Release/chast_viewer.exe`。测试覆盖立即 `Begin`、
33 ms 节流、64 点强制批次、`End` 冲刷、点按圆点、撤销/清除 wire shape、跨
controller 唯一 ID、发送异常恢复、绘制路径完全不调用远程输入回调以及取消路径。

## 6. 未完成验收

自动化已经创建真实可见的 `HostServer` 会话并通过上述窗口、像素与销毁检查；
以下项目仍须真实 GUI 操作或双机环境验收：

1. 在真实查看器窗口中用鼠标、触摸和笔划分别绘制，并目视确认延迟、笔宽与主机
   桌面缩放后的落点。
2. 在真实双机环境确认标注时主机的键鼠不被意外截获，断线后覆盖层立即消失。
3. 在多显示器、负坐标、不同 DPI 和显示器热插拔下确认逐屏裁剪与位置。
4. 同时打开主会话和独立显示器窗口，交替绘制、撤销和清除，确认真实 GUI 与历史
   状态机一致。
5. 在 Windows 10/11、锁屏/UAC 桌面和主机服务模式下确认覆盖层创建、权限与恢复。

在这些环境验收完成前，G21 只能记为“源码完成、自动化/单机 QUIC 与真实
`HostServer` 单机可见覆盖层通过”，不能记为真实双机或生产验收完成。
