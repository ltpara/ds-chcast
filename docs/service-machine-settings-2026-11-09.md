# Windows 服务身份展示与机器设置

日期：2026-11-09。接续 [服务连接管理](service-connection-management-2026-11-09.md)。

后续更新：服务已按交互用户身份自动启动登录会话的管理器，详见 [服务管理器启动](service-manager-startup-2026-11-09.md)。本文末尾的登录自启动待办描述保留本阶段当时的边界。

## 用户可用功能

服务连接管理首页显示当前 Windows 会话工作进程的设备 ID、SHA-256 指纹、实际认证方式、永久密码和两步验证状态。设备 ID 和指纹可复制；后台刷新不会重置文本选择。工作进程失联时清除身份并禁用复制，重连后显示新快照。

“连接服务功能已启用”只描述配置开关，不表示会合服务器当前在线。一次注册成功事件不足以证明持续在线，因此界面没有据此显示在线状态。公开快照不包含密码、访问令牌、TOTP 密钥或设备私钥。本地管理协议版本升级为 2，主机和服务应使用同一套构建。

首页和托盘菜单提供“服务安全设置”和“服务连接设置”。点击后以管理员身份打开同一程序中的机器设置编辑器，复用原生安全/连接设置页面，保存按钮为“保存并重启服务”。编辑期间管理器继续接收审批请求；关闭管理器不会结束独立编辑器或服务。

命令行也可打开页面：

```powershell
chast-host.exe --service-settings
chast-host.exe --service-network-settings
```

两个入口不接受同时指定 `--config` 或 `--portable`，也不能同时选择两种页面。配置文件必须已存在且没有解析警告，不会创建默认配置来覆盖损坏的机器设置。未提升的进程通过 UAC 提升；保存函数还独立检查有效管理员组成员资格。

安全页可以修改永久密码、认证方式、两步验证、可信设备、默认权限及既有高级选项；连接页编辑会合服务器、设备身份、监听地址和代理。Password 模式需要永久密码。新设置的 TOTP 使用机器范围保护，供 SYSTEM 工作进程读取。机器编辑器中的指纹栏指向服务管理首页，不再把缺少本进程 HostServer 误写为“共享未启动”。

未配置文件接收目录时，机器编辑器显示“服务账户默认目录”，避免把编辑器用户的 Downloads 目录误报为服务实际路径；只有用户明确选择目录时才存储该路径。

## 保存语义

保存函数持有仅 SYSTEM/Administrators 可访问的全局互斥体，串行化不同登录会话中的机器设置保存。对于正在运行的服务，先停止并等待工作进程退出，再读取最后的配置，以保留停止期间完成的令牌或可信设备更新。

合并比较打开页面时的原始值、用户编辑值和服务的最新值：

- 只应用当前页面中用户实际修改的字段，其余保留最新值。
- 同一字段被并发改为不同值时，报告冲突且不写入部分修改；两方改成相同值可以合并。
- 可信设备页面只执行撤销，保留运行期间新增的设备和更新后的最近使用时间。
- 修改永久密码清空全部可信设备，包括页面打开后新增的设备。
- 连接页保存不回滚安全设置、访问令牌等无关字段。

原本运行的服务在保存后重新启动，合并/保存失败时也尝试恢复原配置下的运行状态。原本停止或已经处于停止中的服务保持停止。保存成功与服务启动成功分别报告；启动失败不会把已成功落盘的设置错误地报告为“未保存”。服务未正常停止时不会抢先写配置。

服务重启会结束现有远程连接。服务控制管理器的 Running 状态只表示服务进程已启动；媒体采集、当前用户工作进程和会合注册仍各自初始化，管理首页通过实际工作进程管道刷新身份。

## 实现位置

- `host/service_settings.{h,cc}`：字段合并、管理员检查和停止/保存/恢复事务。
- `host/service_control.{h,cc}`、`host/worker.cc`：运行中身份与公开认证状态快照。
- `host/service_manager.cc`：首页、复制、设置入口、编辑器生命周期。
- `host/main.cc`、`host/i18n_en.cc`：机器编辑模式、TOTP 保护范围、中英文界面及命令行帮助。

## 验证

复用现有 `host_settings`、`host_parity` 测试目标，没有新增 CMake 测试项目。合并检查覆盖令牌轮换、新增可信设备、更新时间、撤销、同字段冲突、密码变化、跨页面隔离和无密码的 Password 模式。真实管道/QUIC 场景覆盖公开身份、端点丢失清空和重启替换快照。

`host_settings`、`host_multi`、`host_parity`、`host_security`、`host_i18n` 五项通过，耗时 99.48 秒，见 `build/service-settings-regression.log`。主机、服务及相关测试构建通过，见 `build/service-settings-build.log`。最后的界面与翻译修改另行构建并复查，见 `build/service-settings-ui-build.log`、`build/service-settings-final-host-build.log` 和 `build/service-settings-final-i18n.log`。已有编译器/链接器警告仍存在。

临时验证程序均放在忽略的 `build/`：

- `service-settings-scm-check` 创建唯一命名的临时 SYSTEM 服务并调用产品保存事务。验证机器 TOTP 可由 SYSTEM 解密、正常重启、停止前最后写入的保留、冲突恢复、停止/停止中状态保持、保存成功但启动失败的区分，以及受限调用线程在打开机器配置和服务前被拒绝。日志：`build/service-settings-scm-check.log`。临时服务和配置已清理；未操作正式 `chast-host` 服务或其配置。
- `service-manager-ui-check` 使用实际管道和原生首页/审批窗口，验证公开 ID、复制按钮状态、文本选择保持、端点失联清空，以及既有允许/拒绝逻辑。中英文日志：`build/service-manager-ui-check.log`、`build/service-manager-ui-check-en.log`。
- `service-settings-ui-check` 复用实际安全/连接编辑器，验证接受编辑、TOTP 机器范围保护、底部控件位置和中英文页面。日志：`build/service-settings-ui-check.log`、`build/service-settings-ui-check-en.log`。窗口移到屏幕外渲染，检查图片为 `build/service-overview-*.png`、`build/service-security-*.png`、`build/service-network-*.png`。

以上不等于完整安装、真实普通用户进程的 UAC 交互或多用户切换验收。管理器目前仍只管理同一 Windows 会话的工作进程；用户登录时自动启动管理器、跨用户会话体验、会话内远程提权仍待完成。更广范围的剩余项见 [模块对照](host-rustdesk-module-comparison-2026-11-08.md)。
