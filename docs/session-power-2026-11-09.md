# 远程连接期间防休眠

日期：2026-11-09。参考本地 RustDesk `7423dced37845c48093b6f71bd82e4a16cc8c79d` 中 `start_wakelock_thread` 与 `raii::AuthedConnID::check_wake_lock` 的连接生命周期行为。

## 用户行为

新增 `keep_awake` 本地设置，默认开启。原生主机和服务机器设置的“安全设置 → 访问控制”页提供“远程连接期间防止自动休眠”开关。普通主机保存后按既有流程重启共享；服务机器设置按既有流程保存并恢复服务运行状态。已有配置缺少该字段时使用开启默认值，`keep_awake=0` 可以关闭。

通过认证和审批的桌面连接持有系统、显示器电源请求，因此只观看、不发送键鼠输入的连接也会申请防休眠。文件、终端、摄像头、端口转发等专用连接只持有系统请求。监听等待、尚未完成认证或正在等待审批的连接不持有请求。

每条连接持有独立的 Windows 电源请求。某一查看者离开只释放自己的请求；桌面连接离开但文件连接仍存在时，显示请求释放，系统请求保留。最后一条连接结束、主机停止或采集失败时，请求随会话资源释放。断线后保留的终端本身不算新的远程连接。

该开关是本地主机策略，没有新增远程配置权限或网络协议字段。机器设置合并包含此字段，仍保留其他进程并发更新的令牌、身份和可信设备。

## 实现

`src/win/power_request.cc` 通过 `PowerCreateRequest` 创建每连接的请求，登记 `PowerRequestSystemRequired`，桌面连接另登记 `PowerRequestDisplayRequired`。资源析构匹配清除已登记的类型并关闭句柄。创建或系统请求失败时记录原因并继续连接；显示请求失败时保留已经成功的系统请求，同时记录原因。

`HostServer::serve_current_session` 是普通单连接、并发工作线程和服务工作进程共用的已准入会话入口；请求在该函数内持有，覆盖正常服务及结束清理。现有媒体工厂模式增加电源请求工厂，便于在真实 QUIC 流程内观察资源生命周期，不依靠自动化测试改变本机睡眠状态。

## 验证记录

- Release 主机、服务及既有设置、翻译、主机会话测试目标构建通过；日志 `build/session-power-build.log`、`build/session-power-parity-build.log`。仍有原有链接警告。
- 既有 `host_settings` 覆盖默认值、无效配置、关闭值保存/加载与机器设置合并。既有 `host_parity` 增加一个电源生命周期场景，专项 `--power` 已通过，日志 `build/session-power-focused.log`；覆盖审批等待/拒绝、只观看桌面、文件与终端并发、最后断开、主机停止、普通单连接主机重连、禁用设置、采集失败及电源 API 失败。没有新增 CMake 测试项目。
- 首次回归的新增审批场景遗漏测试用 `service_approval` 标志，未进入审批回调；修正测试设置后专项通过。最终复跑 `host_settings`、`host_parity`、`host_i18n` 全部通过，97.25 秒，见 `build/session-power-regression-final.log`。
- 忽略目录内的 `session-power-native-check` 复用真实主机和 QUIC 客户端，并调用真实电源 API；使用模拟媒体避免操作用户屏幕。`powercfg /requests` 验证空闲、仅文件、文件加只观看桌面、桌面退出后文件保留、全部退出、重连、主机停止、禁用设置时的实际登记。全部通过，见 `build/session-power-native-check.log`。检测限定本辅助进程，未更改其他应用的请求。
- 复用原生机器设置辅助验证，中英文显示和读取/保存均通过。见 `build/session-power-ui-en.log`、`build/session-power-ui-zh.log`；截图 `build/session-power-settings-en.png`、`build/session-power-settings-zh.png` 已检查，新增控件与说明完整可见。

本轮没有修改正式服务或机器设置，没有改变电源计划，也没有触发实际睡眠、合盖或等待系统自动睡眠超时。已验证的是请求登记、连接策略与资源释放；电池模式、Modern Standby、管理员电源覆盖及睡眠/恢复后的完整行为仍需不同机器实测。
