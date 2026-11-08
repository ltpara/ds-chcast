# 按连接 ID 管理观看者

日期：2026-11-09。延续发射端模块对比中“按会话 ID 本地管理”的开发，参考本地 RustDesk 固定提交 `7423dced37845c48093b6f71bd82e4a16cc8c79d`。

> 后续更新：服务工作进程的认证管道、同会话托盘管理和点击审批已接入，见 [服务连接管理](service-connection-management-2026-11-09.md)。下文服务 IPC 尚未接入的表述保留为本阶段完成时的边界记录。

## 产品行为

主机页面和托盘菜单增加“管理连接”入口。窗口列出连接 ID、地址和会话类型，选择后可以修改该连接权限、聊天、发送文件或断开该连接。

- 多连接首次打开时要求明确选择；只有一个连接时自动选择它。之后新连接加入不会改变选择，当前目标离开也不自动改选其他人。
- 每条连接保留自己的聊天记录、输入草稿和未读提示。来自 B 的消息不会覆盖正在给 A 编写的草稿，也不会切换发送对象。
- 文件选择对话框会处理其他窗口消息，因此在打开对话框前就保存目标 ID，选择文件后重新校验该 ID。目标已离开则发送失败，不会转交最近连接的人。
- 权限仍受原批准上限、全局策略与独占键鼠控制权约束；无法通过连接管理扩大批准范围。面板显示传输层实际应用的权限。
- 断开仅结束指定会话，监听器和其他观看者继续工作；单连接顺序监听器也可继续接待下一位。
- 记录最多保留 32 个连接、每个连接 300 行；断开的会话可查阅，但不能继续发送。中文与英文界面均已接入。

## 后端与生命周期

`HostServer::viewers()` 返回不含凭据的已准入连接快照。进程内递增的 `uint64_t` ID 不复用，跨同进程的 HostServer 重建也不同。新增按 ID 的 `set_permissions`、`send_chat`、`start_file_download`、`disconnect_viewer`；不存在、断开或过期 ID 直接失败，绝不回退到其他连接。

本地操作与 ID 发布/撤销共用锁。接收聊天回调 `viewer_chat_message(id, text)` 携带发送者身份。文件专用及终端/摄像头/端口转发专用会话复用聊天收发逻辑。旧的无 ID 接口保留给既有调用方，原生连接管理使用明确 ID；默认主机状态仍描述最近准入者，主界面权限摘要标明该 ID。

会话撤销 ID 时，在同一把锁内读取本地断开标志，避免已经接受的断开操作遗漏于审计记录及关闭超时选择。

修复了顺序会话的文件泄漏风险：原 `end_file_session()` 只清理查看者请求，特意保留尚未发送的本地文件。现在两类文件请求都在会话退出时清理，下一次准入也再次清理队列，避免把 A 的待发送文件交给 B。原有聊天与权限队列也在新 ID 可见前清理。

另外修复了工作线程对象销毁时无条件清除全局 focused 引用的问题：只有监听器销毁时执行整组清理。工作线程离开只影响它自己的会话生命周期。

主要文件：

- `include/chast/host_server.h`、`src/host/host_server.cc`：连接身份、按 ID 操作、聊天收发、退出清理。
- `host/connections.h`、`host/connections.cc`：原生连接选择、权限、聊天和文件操作。
- `host/main.cc`、`host/i18n_en.cc`：页面/托盘入口、带 ID 聊天通知、翻译。

## 对照与边界

RustDesk `src/ui_cm_interface.rs::send_chat(id, text)`、`switch_permission(id, ...)`、`close(id)` 均在连接表中按 ID 路由。本次对齐的是这一操作原则，以及本机原生主机的明确选择流程；不是复刻 RustDesk 的 UI 或 IPC 协议。

服务 worker 已具备同一套 HostServer 后端接口，但托盘尚未通过受控 IPC 连接到服务 worker。当前连接窗口管理托盘进程自己运行的主机；安装服务后的跨进程连接管理、服务点击审批和会话内提权仍需后续开发。尚未审批的连接不列为可操作的已准入会话。

断开请求不在 UI 线程等待 worker 退出；既有同步驱动、设备或文件操作可能延长实际清理时间，不能保证驱动卡死时也立即断开。

## 验证

复用既有 `host_parity` 目标，新增一个真实 QUIC 场景验证：不同会话类型的 ID 和聊天来源；对旧连接回复、撤权/恢复；指定接收端完整接收文件内容；断开一位后其他人继续及名额复用；所有过期 ID 操作失败；顺序模式不断监听、ID 不复用、排队文件不进入下一会话。支持 `chast-host-parity-test --connections` 单独运行。

临时原生窗口验证放在忽略的 `build/`，没有增加 CMake 测试项目。它使用真实产品控件与本机 QUIC，保持窗口隐藏，验证多连接初次选择、新连接/来信不改目标、各连接草稿恢复、Send/Disconnect 的实际路由和断开后禁用输入。中文与英文运行均通过，`WM_PRINT` 生成的两份图像已检查：`build/connection-window-check.png`、`build/connection-window-check-en.png`；对应日志为 `build/connection-window-check.log`、`build/connection-window-check-en.log`。

`chast-host`、`chast-service`、`chast-quic` 及相关既有测试目标构建通过。现有 `host_multi`、`host_parity`、`desktop_stream`、`host_i18n` 共 4 项回归全部通过，总耗时 91.13 秒；日志见 `build/viewer-management-regression.log`，最终构建日志见 `build/viewer-management-verified-build.log`。验证未替代真实多机、物理摄像头或服务跨进程验收。

收尾时修正退出审计竞态后，重新构建上述三个程序及 `chast-host-parity-test`，并复跑 `--connections` 场景通过；日志见 `build/viewer-management-retirement-build.log`、`build/viewer-management-retirement-test.log`。
