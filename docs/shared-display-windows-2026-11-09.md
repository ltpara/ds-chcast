# 同一会话的显示器窗口

2026-11-09，在已有逐屏订阅、解码和光标能力上接入独立 Windows 窗口。协议仍为 v60，主机无需增加连接或编码器。

## 行为

- 显示器菜单的“新窗口”先把目标屏幕加入当前订阅，再按稳定 stream ID 打开 Flutter engine。窗口参数只有 stream ID，连接、认证、隧道仍由主会话管理。
- 重复打开同一 stream 会恢复并聚焦已有窗口。进程仍限制为最多 16 个窗口。
- 每屏只保留一个 `DisplayVideoReceiver` 解码器和 `GpuFrameSink`。GPU 转换只做一次，各窗口持有自己的插件输出；新输出可立即重放最后一帧，静止桌面不用等待视频更新。
- 逐屏光标由共享会话缓存，每个窗口分别记录读取版本，新窗口也能获得当前形状。
- 子窗口支持缩放、全屏和默认关闭的远程键鼠，以及触摸、笔和触控板输入。绝对坐标按该屏源尺寸、原点换算。输入权限仍由原连接和主机控制权约束。
- 只有前台窗口能排队输入。切换输入归属时释放上一窗口的状态；迟到的失焦/关闭请求不能释放另一窗口已经持有的键。
- 子窗口的本地焦点不改变主窗口截图和录制所选的显示器。子窗口不能启动、停止、重新认证或修改整个会话。
- 关闭子窗口同步解绑其输出，再销毁 Flutter engine，其他窗口继续显示。主会话停止、重连或关闭会失效并关闭其子窗口；用户可以在重连后重新打开。
- 主窗口取消某屏订阅或显示器被移除时，该 stream 对应的子窗口关闭，避免保留可控制的旧画面。子窗口本身关闭不会退订主窗口仍在使用的屏幕。

## 窗口位置恢复（2026-11-10）

子窗口按“服务器命名空间下的设备 ID + 主机显示序号”记住上次的位置和大小。关闭时把物理像素矩形写入 `%APPDATA%\chast\display-windows.tsv`，再次打开同一设备的同一屏时恢复；保存的矩形若已不与任何显示器相交（例如拔掉扩展屏），则忽略并回到默认摆放。文件损坏、字段多余或超过 256 条时安全降级，只保留较新的记录。

原生验收程序（`build/tmp_display_layout/`，临时程序不进入仓库）覆盖：二进制设备 ID 往返、同键替换而非追加、畸形行/多余字段拒绝、离屏矩形忽略、256 条上限保留较新项、Roaming 路径解析；Windows 观看端 Release 构建通过。真实双机与多显示器位置恢复仍待人工目视。

## 实现入口

- `app/windows/viewer/shared_viewer_session.{h,cc}`：共享连接、GPU sink、光标缓存、窗口登记、输入归属和会话代次。
- `app/windows/viewer/gpu_frame_sink.{h,cc}`：按窗口 owner 管理 GPU 输出，串行化渲染、重放、截图和重建。
- `app/windows/viewer/viewer_bridge.{h,cc}`：主/子窗口角色、子窗口方法白名单、逐屏状态过滤和同步解绑。
- `app/windows/runner/{main.cpp,flutter_window.h,flutter_window.cpp}`：现有多 engine runner 的显示器窗口入口。
- `app/windows/runner/display_layout.{h,cc}`：按设备与显示序号保存/恢复子窗口物理像素矩形，离屏矩形过滤与 256 条上限。
- `app/lib/pages/display_window_page.dart`：只管理一个显示输出的页面；`session_page.dart` 不再通过 `SessionWindowRequest` 建立第二个连接。

## 验证

- 扩展已有 Flutter 测试文件，没有新增 CMake 测试目标。17 项检查通过，覆盖原会话只启动一次、新窗口按 stream ID 打开、负原点输入、订阅消失关闭、先解绑再释放纹理，以及注册途中关闭和失败后安全重试；`flutter analyze --no-pub` 无问题。
- 构建 Windows Release 查看端并安装 Flutter bundle。
- 复用原有 `display_video`、`viewer_display_streams` 原生回归，2/2 通过（23.99 秒）。
- 临时验收程序 `build/shared_window_acceptance.cc` 直接链接本轮产品的 runner/core 对象及主机库，使用真实 Flutter 页面和 GPU 插件。回环主机限制一个查看者，密码认证和指纹固定；DXGI → OpenH264 → QUIC → D3D11VA → 两个 Flutter engine 实际出图，主机 `stats.sessions=1`，两个窗口复用同一个逐屏 sink。
- 原生验收检查子窗口销毁后继续出帧、暂停采集后新窗口重放最后画面、输入归属切换及旧释放拒绝、父窗口关闭后子窗口退出和旧代次输入拒绝。输入使用主机 `dry_run_input`，不向本机桌面实际注入。
- 日志：`build/shared-windows-native.log`、`build/shared-windows-flutter-test.log`、`build/shared-windows-analyze.log`、`build/shared-windows-final-build.log`、`build/shared-windows-bundle.log`、`build/shared-windows-native-regression.log`。

本机原生验收使用同一真实显示器的两个窗口，证明共享输出与销毁路径；完整 SessionPage 的打开入口由已有 widget 测试覆盖。多块物理显示器、混合 DPI/多 GPU、跨网络及实际键鼠/触控设备操作仍需人工验收。本轮没有扩大为子窗口独立录制或在主会话关闭后接管连接。
