# chast 开发路线图

更新时间：2026-10-25。按依赖顺序排列，已完成项保留在这里作为基线。

## 已完成

- **采集与编码**（提交 fc57d78）：DXGI 桌面复制、旋转、HDR/高级色归一化、光标解码、GPU NV12；Intel oneVPL H.264/HEVC，零拷贝入编码器。
- **传输与显示**（提交 e289d01）：MsQuic 会话、控制流/视频流、FFmpeg D3D11VA 解码、Flutter GPU 纹理显示、断线重连界面。
- **输入 P1**（提交 8416e92）：协议 v2 的 `InputEvent`、`HelloAck` 的 origin 与输入闸门、服务端 `InputInjector`（HID→扫描码、串行注入、卡键释放、dry-run）。
- **输入 P2**：服务端公布真实 origin；`ViewerEngine` 输入队列；`sendInput` MethodChannel；Flutter 指针/按键/滚轮采集与坐标映射；工具栏控制开关，关闭时补发已按下键/鼠标键的释放。
- **输入 P3**：协议 v3 输入状态位图（修饰键 + Caps/Num/Scroll）；注入器对账、锁键点按、AltGr 折叠；`InputRelease` 与失焦/退出/断线全量释放；protocol 单测、回环与 Flutter 测试。
  - 已知限制：自动化环境无法合成真实键盘事件，位图/锁键随物理键盘的端到端回放待人工确认。
  - 设计取舍：不做按键级“按下超时”（会误伤长按），靠 QUIC idle timeout 结束会话后的 held 释放兜底。
- **多显示器 M2.0**（已完成）：协议 v4 的 HelloAck 显示器列表；服务端填充、工具打印、回环三项断言。
- **多显示器 M2.1**（已完成）：协议 v5 的 Hello 携带显示器索引；`serve` 在 Hello 之后才创建采集器并按请求选屏；工具栏显示器菜单与“带 display 参数重连”。
- **会话内重建 M2.2**（已完成）：协议 v6 的 `StreamInfo`；采集重建后重建编码器并重新公告流参数，会话与视频流继续；查看器按更新重建解码器。
- **设备身份 M3.0**（已完成）：服务端在 CNG 密钥容器与证书库中持有持久身份，SHA-256 指纹跨重启不变；客户端默认必须 pin 已知指纹才能连接，Flutter 端按 `host:port` 做首次信任（TOFU）并在指纹变化时拦截。
- **连接审批 M3.1**（已完成）：协议 v7 的 `Hello` 携带访问令牌、`AccessDenied` 返回拒绝原因；服务端校验令牌（默认一次性）并可让操作者逐次审批；Flutter 首页可填令牌，被拒时显示原因并引导修改。

## M2 — 多显示器与会话内重建

- 已完成：`HelloAck` 显示器列表（协议 v4）：每项含名称、虚拟桌面 rect、旋转角度、主屏标志；列表顺序即显示器索引；解析层拒绝未知旋转、空矩形、超过 64 项以及未知标志。`serve` 通过 `enumerate_displays()` 填充，`chast-quic connect` 打印列表，`verify` 断言列表逐项一致、恰有一个主屏、被采集输出在列表中。
- 已完成：显示器选择（协议 v5）。`Hello` 的备用字段改为 `display`（u16，`0xFFFF` 表示交给服务端，上限 `kMaxDisplayCount = 64`）。`serve` 把采集器的创建推迟到 `wait_for_client()` 之后：索引来自 Hello，其次是 `--display`，最后回退主屏；越界请求在握手阶段结束并打印原因。`HelloAck` 的 origin 与画面尺寸因此始终描述真正被采集的那块屏，客户端坐标映射无需额外字段。
- 已完成：Flutter 工具栏显示器菜单。列表来自 HelloAck；选中后释放当前输入并按所选索引重连（“服务端默认”也保留）。“重新连接”沿用上次选择。切换后 origin/尺寸随新的 HelloAck 更新，`_activeDisplay` 在“服务端默认”时用 origin+尺寸反查当前输出。
  - 设计取舍：先做重连而不是会话内切换。一个会话对应一个采集器与编码器，重连不需要在流中插入“重建”控制消息，也不会让客户端在解码器边界上处理参数突变；M2.2 再补会话内重建。
  - 已知限制：索引以上一次 HelloAck 的列表顺序为准，显示器热插拔后同一索引可能指向别的输出；重连会拿到新列表，越界索引由服务端拒绝而不是静默回退。
- 已完成：会话内重建（协议 v6）。`ControlType::StreamInfo = 8` 的载荷与 `HelloAck` 完全相同，由 `chast::make_stream_info()` 生成、`parse_hello_ack()` 解析；视频流、QUIC 流与序号都不变，服务端在更新后强制 IDR。
  - `serve`：`ReinitNeeded` 之后重新创建编码器，不再因尺寸或编码器变化结束会话。新描述来自重新枚举的显示器列表、采集器当前的输出和编码器的实际参数（含编码器回退后的真实编码格式），经 `refresh_stream()` 发送；随后打一帧 IDR。origin、尺寸、旋转、显示器列表、编码器名与码率都随更新到达查看器，指针映射因此继续正确。
  - 客户端：`QuicSession::take_stream_update()` 暴露最近的更新；`chast-quic connect` 与 Flutter `ViewerEngine` 在编码格式或尺寸变化时重建解码器（GPU 模式下仍走 D3D11VA 与同一个 sink），origin/显示器列表/输入闸门同步刷新。帧头与描述不一致时会先查一次更新，避免视频流先于控制流到达的竞态被记成协议违规。
  - 设计取舍：重建后总是重新公告（即使尺寸未变），让客户端状态与采集器严格一致，也省掉逐字段比较；`StreamInfo` 复用 HelloAck 载荷而不是定义新结构，解析代码只有一份。
  - 已知限制：旋转面板若 GPU 无法旋转，编码器仍按原逻辑拒绝该帧并结束会话（与重建前一致）；实际分辨率/旋转变化需要真机操作，自动化环境只能覆盖 `--reannounce` 的会话路径。
- 多显示器输入坐标（负 origin、跨屏）与切换后的回归测试；当前 `verify` 已断言“流参数等于所选显示器的 rect”，两屏以上的真机演练待补。

### M2.1 验证（2026-10-21）

- `chast-protocol-test`：Hello 默认/指定索引往返、越界索引拒绝、旧协议版本拒绝。
- `chast-quic verify`：新增 `the server saw the viewer's display choice`、`the server captured the display the viewer asked for`、`the stream's origin and size are the requested display's rect`；单屏回环全部通过（121 个访问单元）。
- `chast-quic connect --display 0` 实测正常出流；`--display 3` 时服务端报 `the client asked for display 3, but only 1 are attached`，握手按预期失败。
- Flutter：`display_list_test.dart`（列表解析、负 origin 反查、坏条目跳过）与 `display_menu_test.dart`（菜单条目、选择回调、“服务端默认”、列表为空时不可用）。Release 构建同机连接成功，日志为 `capturing display 0 (\.\DISPLAY1)`。
- 未覆盖：两块以上显示器时“菜单切换后画面、origin 同时变化”的真机演练，需要第二块屏幕。

### M2.2 验证（2026-10-21）

- `chast-protocol-test`：新增 `StreamInfo carries the HelloAck payload`（逐字节相同）、`StreamInfo names its control type`、`StreamInfo parses with the HelloAck parser`，以及新参数/新 origin/显示器列表的往返。
- `chast-quic verify`：回环默认在 2 s 时重新公告一次（`--reannounce`），新增 `the server re-announced the stream mid-session`、`the viewer received every stream update`、`the stream update described the running stream`；151 个访问单元、零 CRC/解码/序号/协议错误，摘要 `592344FCFAF7534B`。
- `serve --reannounce` + `connect` 实测：客户端打印 `stream description updated: H.264 1920x1080 at 0,0`，随后继续解码 234 帧，无错误。
- `ctest -C Release`：3/3 通过（cursor、protocol、quic_loopback 5.21 s）。
- Flutter：`display_list_test.dart` 新增“流更新后 origin/尺寸指向另一块屏”；`flutter analyze` 无问题，10 个测试通过，Release 构建成功。
- 未覆盖：真实分辨率/旋转变化触发的 `ReinitNeeded → 重建编码器 → StreamInfo → 客户端重建解码器` 全链路，需要在本机手动更改显示模式后再确认；`--reannounce` 只覆盖协议、会话与客户端状态更新，不解码新尺寸。

### M3.0 验证（2026-10-21）

- `chast-protocol-test`：新增指纹规范化用例——规范形式不变、冒号/小写/空白被接受、63 与 65 位、非十六进制与空串被拒绝；格式化插入冒号；相同指纹匹配、格式化后的 pin 匹配、1 位差异不匹配、非法 pin 永不匹配。
- `chast-quic verify`：回环用本地服务端身份自动 pin，新增 `the viewer connected to the pinned server identity`、`a client pinned to another fingerprint rejects the server`（第二个服务端 + 64 个 0 的 pin，期望 `connect()` 失败）。连续两次 `verify` 打印同一指纹 `C2:F2:…:64:21`，证明身份跨进程稳定。
- `chast-quic connect` 无 `--fingerprint` 时拒绝并提示补 pin 或 `--insecure`；`verify --fingerprint 000…0` 的接收端报 `server certificate: the server's certificate C2:F2:… does not match the pinned fingerprint`。
- Flutter：新增 `trust_store_test.dart`（规范化/短指纹、非法值不写入、按端点存取、损坏文件容错、状态解析），`flutter analyze` 无问题，14 个测试通过，Release 构建成功。
- 真机手动：`serve --port 45634` + Release 查看器 `--connect`，日志打印 `viewer: server identity SHA256 C2:F2:…`，首帧到达，`trust.json` 写入 `{"127.0.0.1:45634": "C2F2…6421"}`；把存储值改成 64 个 0 后重启查看器，连接被拒（错误文案含对端实际指纹），确认 pin 生效；恢复正确指纹后重新出流。
- 未覆盖：身份变化后的“信任并重新连接”按钮点击路径未在自动化里演练（逻辑与存储路径均有单测）；服务账户/Windows 服务下的证书存储、以及 M3.1 的令牌审批待后续阶段。

### M3.1 验证（2026-10-21）

- `chast-protocol-test`：新增 `AccessDenied` 往返（含空原因）、未知 code、长度字段与载荷不符、载荷过短；`Hello` 令牌往返、128 字节上限、截断令牌与尾部多余字节被拒绝。
- `chast-quic verify`：主回环全程带一次性令牌（`the loopback ran with an access token`）；新增 `wrong, spent and declined viewers are refused and told why`，在同一进程里依次验证三种拒绝：错误令牌得到 `permission denied: the access token was not accepted`；第一次查看者被审批回调拒绝、第二次获批（回调恰好调用两次）；一次性令牌被第一位查看者花掉后，第二位收到 `permission denied: the one-time access token was already used`。151 个访问单元、零 CRC/解码/序号/协议错误，摘要 `5A003034AED3A11B`。
- `ctest -C Release`：3/3 通过（cursor、protocol、quic_loopback 16.03 s）。
- 真机手动：`serve --port 45720 --token auto` + Release 查看器 `--connect 127.0.0.1:45720`（未填令牌），查看器日志 `viewer: handshake: permission denied: the access token was not accepted`，服务端继续等待下一位查看者；`serve --approve` 用管道输入 `n`/`y`，第一位查看者打印 `permission denied: the host declined the connection`，第二位获批并解码 91 帧；无令牌的服务端仍可正常出流（181 帧，接收/解码计数与客户端统计一致）。
- Flutter：新增 `access_denial_test.dart`（三种拒绝各有说明、未知 code 无说明、状态解析、令牌随目标传递但不进入 `label`），`flutter analyze` 无问题，18 个测试通过，Release 构建成功。
- 未覆盖：会话页拒接卡片的实际点击（“返回并修改令牌”）与在首页输入正确令牌的完整 GUI 路径未在自动化里演练——桌面输入不可合成，逻辑分别由 `ViewerStatus.denial`/`accessDenialText` 单测与查看器握手日志覆盖；`--approve` 目前只有终端 stdin 提示。

### M3.2a 验证（2026-10-21）

- `chast-host-settings-test`：默认值与缺省文件、往返、逐键非法值（端口/布尔/令牌模式/编码/码率/帧率/质量/显示器）各自回退并带警告、无 `=` 行被报告、令牌字符集与长度、编码别名、非 ASCII 中文路径读写。`ctest -C Release` 4/4 通过（cursor、protocol、host_settings、quic_loopback 16.5 s）。
- 托盘实测（单屏 1920x1080，配置文件在 `build/tray-test/`）：
  - 首次启动生成带注释的默认 `host.ini`（`bind=0.0.0.0`、`port=4433`、`input=1`、`approve=1`、`token_mode=one_time`、随机令牌），第二个默认配置实例被互斥体挡住并直接退出（退出码 0）。
  - 一次性令牌：`chast-quic connect` 用当前令牌成功出流（692 帧、零 CRC/解码/序号错误）；会话结束后 `host.ini` 里的令牌自动换成新值并出现在托盘令牌框；用旧令牌再连得到 `permission denied: the access token was not accepted`，且监听器继续等待。
  - 连续会话：8 次“查看者中途被杀”的循环后托盘仍处于监听、令牌每次都轮换、每轮都重新创建采集器与编码器，无泄漏迹象（内存回到 ~30–90 MB 区间）。
  - 会话中执行“停止共享”：查看器被断开，托盘回到“已停止”，日志记录查看器离开；再点“开始共享”后新令牌连接成功。
  - 审批：开启“连接前需要确认”后，查看器连接时弹出 `chast 连接请求` 对话框（含对端地址）；选择“否”时查看器收到 `permission denied: the host declined the connection`，托盘记录“已拒绝来自 … 的连接”；选择“是”时正常出流（370 帧）。
  - 开机启动：`--autostart on` 后注册表出现 `"<exe>" --tray`，`off` 后值被删除。防火墙：`--firewall 45461` 返回 0，`netsh advfirewall firewall show rule name="chast host" verbose` 显示 UDP、本地端口 45461、程序指向 `chast-host.exe`、域/专用/公用三档全开。
  - 端口冲突：第二个实例用同一端口启动时，托盘日志记录 `启动失败：QUIC ListenerStart failed on …: 0x80072740` 并弹出错误框，进程不崩溃。
  - 修复验证：会话收尾阶段的 `StreamSend` 返回 `E_ABORT` 不再显示为 `capture failed`；正常结束日志为 `the viewer left`，被杀为 `the viewer disappeared`。
- 未覆盖：托盘图标的鼠标点击（左键显示、右键菜单）与各按钮的真实鼠标路径由 `WM_COMMAND` 等价调用验证，未做物理点击；高 DPI 缩放、多显示器下的默认显示器设置未实测；锁屏/未登录场景留给 M3.2b 的服务。

## M3 — 服务端产品化与安全

- 已完成：设备身份 M3.0。服务端首次启动生成 RSA 密钥（CNG 容器 `chast-quic-key`）与自签名证书（`CN=chast-device`），存入当前用户的 `My` 证书库；之后每次启动复用同一密钥容器下最新的有效证书，因此指纹跨重启、跨进程不变。指纹是证书 DER 的 SHA-256，以小写也可输入的 64 位十六进制往返，日志与 UI 显示为带冒号的大写形式。
  - 客户端：`QuicClientConfig` 以 `pinned_fingerprint` 指定期望身份；MsQuic 使用 `INDICATE_CERTIFICATE_RECEIVED | DEFER_CERTIFICATE_VALIDATION`，回调里比对指纹后才 `ConnectionCertificateValidationComplete` 放行或拒绝。没有 pin 又不是显式 `allow_any_certificate` 时连接直接失败——旧的“默认不校验证书”已删除。`allow_any_certificate` 只服务于首次接触（此时 UI 从状态里读出指纹并落盘），工具里对应 `--insecure`。
  - Flutter：`TrustStore` 把每个 `host:port` 的指纹存到 `%APPDATA%\chast\trust.json`；首次成功握手写入（TOFU），之后每次连接都 pin。身份不匹配时原生层拒绝连接、状态里仍带对端实际指纹，断线卡片显示新旧指纹并要求“信任并重新连接”；统计浮层显示 `SHA256` 短指纹。
  - 设计取舍：自签名证书 + 指纹 pin 而不是搭一套 CA —— 对直连的两台机器，指纹就是最短的信任锚，也避免客户端依赖系统信任链。身份持久化放在用户证书库而不是每次新建，换来跨重启稳定的指纹，否则 pin 无从谈起。pin 按端点保存而不是全局，同一台服务器换地址需要重新确认一次。
  - 已知限制：身份属于当前 Windows 用户；服务账户的机器级身份已由 M3.2b 补齐（`LocalMachine` 密钥与证书）。指纹校验不做证书链或有效期检查（自签名场景下无意义）。
- 已完成：连接审批与一次性令牌 M3.1（协议 v7）。
  - 协议：`Hello` 增加访问令牌（`u16` 长度 + 字节，上限 128）；新增 `ControlType::AccessDenied = 9`，载荷为 `u8` 拒绝代码（1 令牌无效、2 令牌已用、3 主机拒绝）+ `u16` 长度的原因。令牌只在 TLS 已认证的 QUIC 连接内传输，服务端做常量时间比较。
  - 服务端：`QuicServerConfig::access_token` 与 `reuse_access_token`；第一个被接受的 Hello 花掉令牌（`reuse` 时保留），之后再出现同一令牌即判 `TokenUsed`。`approve_client` 在令牌通过后由 `wait_for_client()` 在应用线程调用，返回 false 时发送 `Declined`。所有拒绝走同一条路：先发 `AccessDenied` 并等待发送完成，再等对端自行断开（最多 750 ms），才关闭连接并复位会话状态（含 `transport_down`/`transport_failed`、握手字段与令牌外的所有会话状态），监听器随即接受下一位查看者。`wait_for_client()` 在“已连接但未握手就离开”时同样复位后继续等待，而不是结束整个等待。
  - 工具：`serve --token <值|auto> [--token-reuse] [--approve]`、`connect --token <值>`；`--approve` 在服务端终端逐次询问 `allow? [y/N]`；`auto` 生成 8 位无歧义字符（去掉 0/O/1/I）。
  - Flutter：首页新增“访问令牌”输入，只随本次连接发送，不写入最近连接或 `trust.json`；`ViewerEngine` 与 bridge 把它放进 `QuicClientConfig::access_token`；握手被拒时 `ViewerStatus::denial` 带上拒绝代码，会话页显示“服务端拒绝了连接”、对应的中文说明与服务端原文，主按钮变为“返回并修改令牌”。
  - 设计取舍：令牌放在 Hello 内而不是单独的握手轮次——TLS 已经认证了服务端身份，令牌只负责准入；默认“一次性”让短期令牌在成功后立即失效，`--token-reuse` 才退化为固定口令。审批放在应用线程而不是 MsQuic 回调里，既能阻塞等待操作者，也避免在回调中关闭连接；拒绝后的短暂等待换来客户端稳定收到拒绝原因，而不是只看到传输断开。
  - 已知限制：令牌没有过期时间，只在被花掉或服务端重启后失效；拒绝原因由服务端下发英文文本，UI 用代码给出中文、原文仅作补充；同一时刻仍只服务一个查看者，拒绝后的复位最坏要等对端断开或 750 ms。
- 已完成：托盘主机与设置持久化 M3.2a。
  - 设置：`chast::HostSettings` 把端口、监听地址、输入、审批、令牌模式与值、编码格式/码率/帧率/质量、开机自启、默认显示器集中到一个平面 `key=value` 文件。每项独立回退并累加警告，手改坏的值只会被换成默认值而不会让服务端起不来；保存走 `<path>.tmp` + `MoveFileEx(REPLACE_EXISTING|WRITE_THROUGH)`，断电也留不下半个文件。路径按用途分两处：托盘用 `%APPDATA%\chast\host.ini`（当前用户），未来服务用 `%ProgramData%\chast\host.ini`（机器级）。
  - `HostServer`：把 `tools/quic.cc` 里的“监听 → 握手 → 采集 → 编码 → 发送”整段收进库，命令行与托盘跑同一份代码。一个实例持有一个监听器并按顺序服务查看者；会话结束后复位连接并继续监听，一次性令牌在两次会话之间轮换（`token_changed` 回调让托盘落盘）。`serve_once`/`run_seconds`/`reannounce`/`keyframe_interval_ms` 等原本写死在工具里的策略变成配置项，工具只负责打印与回环断言。
  - 托盘 `chast-host`（Win32 + 通知区域）：显示状态、查看器地址、流参数与实时帧数、访问令牌（复制/更换）、身份指纹、四个开关（远程输入、连接前确认、一次性令牌、开机自启）、防火墙按钮、设置文件按钮、开始/停止、日志窗。审批改为 `MessageBox` 逐次询问并记录允许/拒绝；关闭窗口只隐藏，退出在托盘菜单。`--tray` 供开机启动使用，`--config` 换设置文件，`--autostart on|off` 与 `--firewall <port>` 是非交互入口（后者即提权子进程）。同一用户下默认配置只有一个实例（`Local\chast-host-tray` 互斥体 + 已有窗口前置）。
  - 系统集成：开机启动写 `HKCU\...\CurrentVersion\Run` 的 `chast-host` = `"<exe>" --tray`；防火墙用 `netsh advfirewall` 添加一条按程序 + UDP 端口限定的入站规则，先删同名旧规则再加，避免换端口后留下多条。托盘通过 `ShellExecuteExW` 的 `runas` 重新拉起自己执行 `--firewall`，UAC 取消时只记一条日志。
  - 设计取舍：设置不放注册表而放文本文件——可直接编辑、可复制排障、跨进程（托盘/服务）共享同一套解析与校验代码；托盘与服务共用 `HostServer` 而不是各写一份循环，保证“命令行验证过的行为就是产品行为”。默认监听 `0.0.0.0` 且默认开启远程输入，因为输入闸门已有令牌 + 审批两层准入；不想暴露时把 `bind` 改成 `127.0.0.1`。
  - 修复：MsQuic 在连接收尾时 `StreamSend` 可能返回 `E_ABORT`/`E_NOT_VALID_STATE`，此前被映射成 `CaptureFailed` 并在托盘日志里显示成 `send_frame: capture failed:`；现在按“对端已离开”处理，日志只报查看器离开或消失，不计入失败。关闭共享导致的收尾错误也不再被当成会话失败。
  - 已知限制：托盘只服务当前登录用户，锁屏/未登录时不工作（无人值守场景由 M3.2b 的 Windows 服务覆盖）；身份证书在用户证书库，服务账户的机器证书由 M3.2b 提供；托盘没有单实例跨会话（不同用户各一份）；界面为固定布局，只按 DPI 缩放，未做自适应重排。
- 已完成：Windows 服务主机 M3.2b（`docs/m3.2b-plan.md`）。
  - 安装与控制：`chast-service.exe --install/--uninstall/--token/--run/--worker`，服务名 `chast-host`，LocalSystem、延迟自动启动、失败后 5/10/30 s 重启；安装时初始化 `%ProgramData%\chast\host.ini`、移除当前用户 HKCU 自启动、写入分别指向 `chast-service.exe`/`chast-tunnel.exe` 的两条 UDP 入站规则；卸载删除服务与规则、保留设置和日志。
  - 会话工作进程：服务发现 `WTSGetActiveConsoleSessionId` 对应的已登录会话，复制该会话 `winlogon.exe` 令牌并以 `winsta0\default` 启动 `--worker`（SYSTEM，用户会话），由它运行 `HostServer` 与 `chast-tunnel host`。停止事件在 `Global\` 命名空间，服务级 job 管住工作进程，隧道另有 `KILL_ON_JOB_CLOSE` 子作业；工作进程被强杀后服务 3 s 重启，`max_consecutive_failures=0` 让锁屏等采集失败无限重试。
  - 机器身份与设置：CNG 机器密钥容器 + `LocalMachine\My` 自签证书，指纹跨重启、跨用户稳定；服务模式取消审批对话框，仅访问令牌准入，一次性令牌轮换后写回机器设置，`--token` 供操作者读取。托盘检测到服务运行后提示并退出。
  - 验证（2026-10-22，单机回环）：`ctest` 5/5、`go test ./...`、Release 全目标通过；服务托管下直连 653 帧、中继 691 帧，CRC/解码错误为 0；强杀工作进程测试暴露“隧道继承 QUIC 套接字导致 4433 被残留隧道占用”的缺陷，修复句柄白名单与隧道子作业后，3 s 恢复、新隧道上线、指纹不变，经中继回放 703 帧、0 错误；托盘让位、停止后注册 55 s 过期、重启自动上线、卸载清理均实测通过。
  - 设计取舍：服务只做生命周期，采集/编码/输入留在交互会话；身份放机器证书库而不是服务账户的用户库，保证卸载重装与不同登录用户看到同一设备；隧道生命周期绑定工作进程，宁可被一起杀掉也不留下占端口的孤儿。
  - 已知限制：锁屏/登录/UAC 安全桌面本身仍无法采集与注入，本阶段只保证服务与工作进程在锁屏期间存活、解锁后恢复；注销/切换用户与锁屏恢复需人工复核；同一时刻只服务活动控制台会话。
- 若要在不可信网络使用，M3.1 必须排在 M4 之前。

## M4 — 连接与地址簿

- 已完成：Go 服务端与隧道代理 M4.0（`server/`）。
  - 服务端 `chast-server` 提供三件事：HTTP 注册表（`POST/GET/DELETE /v1/hosts`、`/healthz`、`/v1/stats`）、UDP 会合端口和按主机分配的 UDP 中继端口。参考 RustDesk `hbbs`/`hbbr` 的信号流程，但没有照搬“按 uuid 配对两条 TCP 流”：chast 的传输是 MsQuic QUIC，TCP 中继必须终止 QUIC 或改传输层，所以这里改为**透明转发 UDP 数据报**，每个主机一个中继端口，查看者把该端口当作普通 QUIC 服务端地址，TLS、令牌、审批和指纹 pin 仍然只在两端生效，服务器无法解密。
  - NAT 穿透由两侧 `chast-tunnel` 代理完成：MsQuic 不暴露内部 UDP 套接字，代理各自持有一个对外数据套接字，用它保活、打洞并转发真实数据。服务器只根据会合端口观察到的地址交换候选，不信任客户端自报；双方从同一套接字互发探针，锥形 NAT 可直连，对称 NAT 则失败。查看者 `auto` 模式在 `--direct-timeout` 后切到中继并保持，`direct`/`relay` 用于强制验证单条路径；因为切换前后代理的五元组不变，QUIC 不需要重连逻辑。直连时数据不经过服务器。
  - 中继端口沿用 `HostServer`“同一时刻一个查看者”的语义：活跃查看者存在时其他来源收到 `busy`，空闲 60 s 或主机 90 s 无流量即回收；主机必须先用私钥签名通过 `relay_hello` 挑战绑定端口（M10.1），未认证来源不能改绑。会合端口对 `punch_request` 按 IP 限速，中继按原样转发、不放大 QUIC 初始包。
  - 代理协议是 `CHASTC1\n` + JSON 的 UDP 控制报文，与不透明数据靠魔数区分；注册只上传 Ed25519 公钥与签名（M10.1），`--server-key` 保护注册、统计和下线，重复注册 ID 但摘要不符会被拒绝。
  - 验证（2026-10-21，单机回环）：真实 chast 端到端在直连模式下 `frames=201 ... received=201 decoded=201 digest=E198527DF2B2B548`，同期中继字节为 0；强制中继模式下 `frames=202 ... received=202 decoded=202 digest=90C686753B88E5A2`，中继端口统计 `to_host_bytes:16402, to_viewer_bytes:2501405`。`go test ./...` 覆盖控制报文、注册表、HTTP API、会合/中继及代理三种模式（真实 UDP 回显 + 进程内服务器），并验证未知主机、错误密钥、第二查看者 `busy` 等失败路径。命令与数据见 `server/README.md` 的“验证记录”。
  - 设计取舍：不做账号体系，主机 ID + Ed25519 公钥就是最小注册凭据（M10.1）；中继不合并多个主机到一个端口，换取“服务器重启后按主机重新分配”的简单性与不混淆数据报的隔离性；直连失败是显式回退而不是伪装成功。
  - 已知限制：单机无真实 NAT，跨网锥形 NAT 打洞与对称 NAT→中继仍需两台异网机器手动复核（回退决策本身有单元测试）；中继是 UDP，无拥塞控制，弱网表现取决于 QUIC 自身；HTTP 默认明文，公网部署需在前置 TLS 反向代理后。
- 已完成：设备地址簿与内置查看者代理 M4.1（`docs/m4.1-plan.md`）。
  - 查看者：Flutter 会话页通过 `TunnelAgent` 托管 `chast-tunnel viewer` 子进程，代理监听 `127.0.0.1:0` 并把实际端口经 stdout `ready` JSON 事件交回；`path` 事件在直连/中继切换时更新会话浮层。代理随 Flutter Windows 构建一起生成，Release 目录中 `chast_viewer.exe` 与 `chast-tunnel.exe` 并存。
  - 地址簿：`DeviceStore` 用一份 `%APPDATA%\chast\devices.json` 同时保存“我的设备”（`saved=true`）和最近连接（未保存、上限 8 条）；首页支持添加/改名/删除、从最近连接保存、清空最近不影响地址簿。服务器 URL 与 `auto/direct/relay` 策略保存在 `server.json`，`GET /v1/hosts/{id}` 用于在线状态、候选/中继地址与指纹。
  - 信任：经服务连接时按逻辑身份 `server:<url>/<id>` 做指纹 pin，而不是代理的随机 loopback 端口；直连设备仍按 `host:port`。首页侧栏显示连接服务、路径策略、在线设备数和已固定身份数，替换原“身份与中继将在后续接入”占位。
  - 代理协议：`ViewerConfig.OnPath` 与 `PathInfo` 在 Go 侧报告初始路径和回退；stdout 只输出机器可读 JSON，诊断留在 stderr。查看者重连时新 loopback 源端口可接管旧客户端，代理可被同一会话页复用。
  - 验证（2026-10-22，单机回环）：真实 chast 经 Dart `TunnelAgent` 直连 `received=201 decoded=201 keyframes=1 crc-errors=0 decode-errors=0 gaps=0 digest=A188ABD0B85F082B`，中继计数全 0；强制中继 `received=202 decoded=202 digest=E092051892076F56`，中继统计 `to_host_bytes:18054,to_viewer_bytes:2575942`。`go vet ./...`、`go test ./...`、`flutter analyze`、50 项 `flutter test` 与 Release 构建全部通过。
  - 设计取舍：继续复用 Go 代理而不是在 Dart/C++ 重写打洞，避免两套 NAT 逻辑分叉；代理用 stdout 事件而不是解析日志，嵌入方只依赖稳定字段；地址簿与最近连接共用一个文件，保存只是改 `saved` 标记。
  - 已知限制：服务器设置对话框的桌面截图因应用被视频窗口遮挡未补，行为由 widget 测试覆盖；跨网 NAT 与物理鼠标路径仍属环境限制。（主机端自动注册已由 M4.2 补齐。）
- 已完成：主机端隧道集成 M4.2（`docs/m4.2-plan.md`）。
  - 托盘：`chast-host` 在 HostServer 监听成功后启动随包的 `chast-tunnel host` 子进程，停止共享/重载/退出时先停代理；设置对话框新增连接服务 URL、服务器密钥、设备 ID/设备密钥、隧道监听与启用开关，首次启动自动生成稳定设备 ID 和 16 字符设备密钥。代理异常退出置为离线，可“重新注册”。
  - 事件与路径：主机代理 stdout 只输出 `ready`（设备 ID、监听、会合/中继地址）、`relay`（中继地址刷新）、`viewer`（路径、远端）；主机侧按数据报来源是否等于分配到的中继端点判定 `direct`/`relay`，托盘仅在串流时显示“最近路径”，查看者离开后清除。C++ 只解析扁平字符串字段，`CHAST_TUNNEL` 可覆盖代理路径。
  - 构建与防火墙：CMake 调 `go build` 产出与 `chast-host.exe` 同目录的 `chast-tunnel.exe`；防火墙按程序各建一条 UDP 入站规则（主机 QUIC 端口 + 隧道端口），禁用服务时删除隧道规则。
  - 验证（2026-10-22，单机回环）：托盘自动注册后 `/v1/hosts/m42-host` 返回在线、候选 `127.0.0.1:16434`、中继 `127.0.0.1:41100` 与指纹；直连 `received=603 decoded=603 gaps=0 digest=610B8EF3346AD47C`，中继 `received=603 decoded=603 digest=659B01211DD715C6` 且中继统计 `to_host_bytes:35677,to_viewer_bytes:10594896`；托盘分别显示“最近路径：直连/经中继”。`ctest -C Release` 5/5、`go test ./...` 全过；退出托盘后子进程消失、注册过期下线；两条防火墙规则实测生效后删除。
  - 设计取舍：继续复用 Go 代理与 stdout 事件，托盘不引入 JSON 库；代理只报告主机侧观察到的路径事实，由托盘按串流状态决定是否展示。
  - 已知限制：单机无真实 NAT，跨网打洞/对称 NAT 仍待两台异网机器；锁屏/未登录的安全桌面采集仍不在范围内（M3.2b 只保证服务存活与解锁后恢复）；设置对话框的鼠标点击路径未自动化。
- 下一阶段：M5 其余体验扩展（音频、文件传输等）。

## M5 — 体验扩展

- 已完成：自适应码率与 RTT/端到端延迟 M5.0（`docs/m5.0-plan.md`）。
  - 协议 v8：`FrameHeader` 复用 7 字节保留区存放 48 位 `capture_ms`（头仍 48 字节）；新增 `LatencyProbe`/`LatencyReply`，载荷长度严格校验。主机在同一控制流自动应答探针，客户端据此计算 RTT 与两端时钟偏移。
  - 自适应码率：新增纯逻辑 `BitrateController`，按丢包率、RTT 膨胀（相对 min RTT）、发送队列/拥塞窗口压力决策；配置码率为上限，最低 `max(500 kbps, 上限/10)`，下调冷却 1.5 s、恢复冷却 3 s，健康 3 s 后按 `max(100 kbps, 上限/50)` 回升。主机在帧间应用 `set_bitrate`，强制 IDR 并重公告 `StreamInfo`。
  - 遥测：`QuicSession::transport_stats()` 读取 MsQuic `QUIC_STATISTICS_V2`（V1 回退）；查看者浮层显示 RTT、帧龄与目标码率，托盘画面行显示自适应码率与 RTT；`chast-quic connect` 输出 RTT/偏移/帧龄，`verify` 将它们纳入回环断言。
  - 验证（2026-10-22，单机回环）：`chast-quic verify --port 45435 --seconds 5` 126 帧全部断言通过，RTT 0.4 ms、时钟偏移 0.1 ms、帧龄 7.8 ms（最大 10.3 ms）、目标码率 2777 kbps、ABR 变化 0；`ctest -C Release` 6/6、`go vet`/`go test`、`flutter analyze`、`flutter build windows --release` 全部通过。
  - 设计取舍：探针 1 Hz 且应答走非阻塞发送，队列满时丢弃而不阻塞传输线程；帧龄按“采集→收到”计算，不含本地解码/呈现；不做基于发送速率的下调，避免把屏幕内容的 VBR 误判为拥塞。
  - 已知限制：单机回环无丢包/排队，降码率路径仅由单元测试覆盖；分辨率/帧率自适应、限速器与跨网弱网演练留待双机阶段；浮层与托盘未做人工鼠标观察。
- 已完成：剪贴板文本双向同步 M5.1（`docs/m5.1-plan.md`）。
  - 协议 v9：`ClipboardUpdate` 控制消息（`seq` + UTF-8 文本，1..256 KiB），解析拒绝空串、NUL、超长、非法 UTF-8 与代理对；`HelloAck`/`StreamInfo` 的 bit1 公告剪贴板可用，未知 flags 位仍拒绝。
  - 会话与主机：`QuicSession` 双向发送/收取，服务端闸门 `allow_clipboard` 默认关，`HostServer` 按设置打开；Win32 `CF_UNICODETEXT` 后端（UTF-8↔UTF-16、`OpenClipboard` 重试）由主机每 500 ms 轮询，连接时只记基线不回传历史，写入远端文本后更新基线防回声，失败只跳过本轮。
  - 查看器：`ViewerEngine` 暴露队列与计数，bridge 新增 `sendClipboard`/`takeClipboard`；Flutter 会话页 500 ms 轮询本机与远端并抑制回声，浮层显示开关与收发计数；`host.ini` 新增 `clipboard=0/1`（默认开），托盘复选与菜单项随设置持久化并重启共享。
  - 验证（2026-10-22，单机回环）：`chast-quic verify` 57 项断言全过（含 8 项剪贴板断言与关闭闸门探针：主机/查看者各收 1 条、各发 1 条、内容逐字相等、计数为 1 证明无回声），145 个访问单元；真实 Win32 剪贴板双向实测（主机→查看者、`connect --send-clipboard`→主机各一次）；`ctest -C Release` 6/6、`flutter analyze`、53 项 `flutter test`、`flutter build windows --release`、`go vet`/`go test` 全部通过。
  - 设计取舍：复用可靠有序的控制流而不新开流；两端都以“变化”为准并轮询而非剪贴板监听链，换取两端一致与简单，代价最多 500 ms 延迟；畸形消息计入传输错误但不关闭会话，避免剪贴板拖垮视频。
  - 已知限制：只同步纯文本，图片/文件/HTML 与历史记录不在范围；超过 256 KiB 的文本静默不同步（待分片）；快速连续复制只保留最后一次稳定文本；Windows 以外的剪贴板后端未实现；未做真实鼠标操作的复制粘贴人工验收，Win32 路径由真实回环与 PowerShell 读写覆盖。
- 已完成：系统声音单向传输 M5.2（`docs/m5.2-plan.md`）。
  - 协议 v10：新增 48 字节 `AudioHeader`（`CAUD` magic、独立序号、`pts_ms`/`capture_ms`、载荷 CRC32、采样率/帧长/声道/codec）与 `AudioParser`；`ServerInfo` 在显示器列表后追加音频参数块，flags bit2 公告可用，extradata 上限 64 字节；HelloAck 与 StreamInfo 逐字节相同。
  - 传输：主机在 `announce()` 时按闸门打开第二条单向流，查看者按首 4 字节 magic 分类，不依赖开流顺序；音频发送使用非阻塞路径，队列满只丢音频包并计数，不阻塞视频；闸门关闭时 `send_audio` 返回 `InvalidCall`。
  - 采集与编码：WASAPI loopback 共享模式 + 事件回调，设备混音格式统一重采样/下混到 48 kHz 立体声 s16；FFmpeg 原生 AAC-LC（48 kHz、160 kbps、1024 样本/帧，静态链接）；采集/编码在独立线程按设备节奏运行，主机循环只负责 drain 与发送。
  - 播放：查看者 AAC 解码后按“主机 capture_ms − 时钟偏移 + 80 ms lead”排程，WASAPI 共享渲染；端点缓冲深度计入提前量，启动时以静音铺垫 lead、并允许提前一个端点缓冲提交；欠载补静音，陈旧块丢弃，缓冲/欠载/丢块进遥测与浮层。
  - 降级：启动时打不开设备或采集/编码终端失败只关闭音频并通过 `StreamInfo(audio.enabled=false)` 重公告，视频继续；运行中端点失效或默认输出变化原地跟随新默认端点恢复，不中断音频流；查看者协议错误的音频包只计数并丢弃。
  - 设置与 UI：`host.ini` 新增 `audio=0/1`（默认开），托盘“共享系统声音”复选与菜单随设置持久化；查看者工具栏扬声器按钮切换静音，浮层显示音频格式、缓冲、收包与欠载（恢复期间显示“设备恢复中”，恢复次数随遥测显示），静音时显示“已静音”。
  - 验证（2026-10-22，单机回环）：`chast-quic verify` 72 项断言全过（含 14 项音频断言 + 关闭闸门探针），151 个访问单元；真实 WASAPI 采集 25 s 为 1170 包/24.94 s、零丢包零错误、RMS 574；真实 WASAPI 渲染 20 s 为 `played=948224 frames underruns=1 dropped=9`、收包 936/19.95 s、零 CRC/解码/序号错误；`ctest -C Release` 7/7、`flutter analyze`、57 项 `flutter test`、`flutter build windows --release`、`go vet`/`go test` 全部通过。
  - 设计取舍：独立单向流避免音频被大视频帧队头阻塞；固定 48 kHz 立体声上行让线上格式与设备解耦，端点切换无需重新协商；AAC-LC 复用静态 FFmpeg，零新增运行时依赖；采集线程与视频循环解耦；时间线统一用主机 steady_clock，与视频帧龄、LatencyReply 同一时钟域。
  - Windows 健壮性补强（2026-10-22）：连续数字静音超过 2 s 停止编码（`gates` 遥测）；采集/播放起播、欠载与门控恢复后 5 ms 线性淡入；采集与播放每秒检查默认端点，失效或变化时原地重开并重试，`recoveries`/`recovering` 进遥测与 `chast-quic` 输出；修复 `push()` 与端点重建的格式竞争及静音的跨线程 COM 调用。
  - 补强验证：`ctest` 7/7；`verify` 72 项断言全过；数字静音 6 s 回环 `gates=2`、静音期间只编码约 2 s、收包 654/13.93 s 且零 CRC/解码/序号错误；真实播放 12 s 为 `played=668672 frames underruns=1 dropped=1 recoveries=0`。
  - 已知限制：单机回环无真实网络抖动；AAC + 80 ms 抖动缓冲相对视频晚约 50–100 ms，未做精细唇音校准；>2 声道下混为立体声；端点切换需改系统默认设备，单机无法触发，恢复路径仅代码审查；静音门控在“无声即不产包”的驱动上不会被触发（但本就没有流量）；托盘与浮层未做人工鼠标验收。
- 已完成：远端指针（原生光标）M5.3（`docs/m5.3-plan.md`）。
  - 问题：Desktop Duplication 不把指针画进桌面纹理，捕获侧虽然早已解码并缓存指针形状（`CursorDecoder` + `cursor_content_id`），却没有任何通路把它送到查看者；查看者远程控制时又设了 `SystemMouseCursors.none`，于是远程指针完全不可见。
  - 协议 v11：新增 `CursorUpdate = 13`（服务端→客户端），26 字节定长头（visible / x / y / 热点 / 53 位 shape_id / 宽高）+ 可选预乘 BGRA 精灵；解析拒绝截断、>256 px、未知 flags、尺寸不符、尾随字节与热点越界。
  - 传输：`QuicSession::send_cursor` 走控制流且非阻塞（队列满即丢并计数，绝不拖慢视频），`take_cursor_update` 采用“最新覆盖”而不是排队；主机按 `content_id` 去重，只有新形状才带精灵，位置/可见性不变则完全不发。
  - 查看者：新增 `RemoteCursor`（`CreateDIBSection` + 按 **alpha** 生成 AND 掩码 + `CreateIconIndirect`，`DestroyCursor` 释放），通过 `WM_SETCURSOR(HTCLIENT)` 安装为系统光标——由 OS 在硬件光标频率合成，不落后、不漂移；精灵按“绘制比例 × 设备像素比”缩放，1/4 步进量化并缓存 (形状, 比例) 句柄，形状表上界 64、每形状句柄上界 12。
  - 分层：精灵不经过 Dart。`ViewerStatus` 只增加 `cursorUpdates`/`cursorPending`；`ViewerEngine::TakeCursor` 由原生桥接每 200 ms 直接取走，Dart 只提供 `enabled` 与 `scaleMilli`。
  - 验证（2026-10-22，单机）：`chast-protocol-test` 新增约 10 项 CursorUpdate 断言、新增 `chast-remote-cursor-test`（缩放量化、注册校验、句柄复用、越界缩放、容量淘汰）均通过；`ctest --test-dir build -C Release` 8/8、`flutter analyze` 无问题、`flutter test` 59 项全过、`flutter build windows` 成功。
  - 设计取舍：安装系统光标而非在视频上叠画精灵（远程控制时位置由本地手决定，只有系统光标能做到同频）；掩码取自 alpha 而非黑色像素（Flutter 引擎自带的 `createCustomCursor/windows` 会把描边指针的黑边当透明抠掉，`flutter_custom_cursor` 0.0.4 的 Windows 实现是硬编码示例图的桩，均不可用）。
  - 已知限制：本机默认没有可见硬件指针（`GetCursorInfo` 为 `flags=0, hCursor=0`），用 `ShowCursor(TRUE)` 强制显示后回环得到 `sender: cursor sent=257 shapes=15 bytes=64080 dropped=0` 与 `receiver: cursor updates=229 shapes=11 visible=1`，`chast-capture --cursor` 导出 `32x32 color` 精灵；但**查看者窗口内肉眼可见的指针仍需一台有物理鼠标的机器人工确认**（安装器已由 `GetIconInfo` 验证到 64×64 与热点 (6,10)）；仅实现 Windows 查看者路径。

## M6 — 弱网 QoS 深化

- 已完成：帧率阶梯与音频预算 M6.0（`docs/m6.0-plan.md`）。
  - 问题：M5.0 的码率控制器只有一个杠杆。码率降到 500 kbps 时 1080p 每帧只有约 0.008 bpp，画面碎成色块；而系统声音是恒定 160 kbps，视频却仍按整个配置码率发送，两者相加会超出运营者配的预算。
  - 帧率阶梯：`BitrateController::framerate_for()` 按“目标 / 上限”的比例分四档——≥70% 满帧率、45–70% 2/3、25–45% 1/2、<25% 1/3，下限 10 fps；它是目标码率的纯函数，恢复时自动跟随，不会出现“码率回来了帧率还卡在 10 fps”。`BitrateDecision` 每次都携带帧率，漏掉某次变化的调用方仍能拿到与当前目标一致的速率。
  - 音频预算：新增 `video_floor()`/`video_budget()`，上限 = 配置码率 −（音频码率 × 1.1），预留夹在半额以内，下限仍按 1/10 兜底；音频未启用时全额留给视频。控制器从构造起就以预算为天花板，所以“恢复”也停在“视频 + 音频 = 配置预算”。
  - 编码器：`set_bitrate()` 升级为 `set_rate(bitrate, framerate)`，一次 `MFXVideoENCODE_Reset` 同时改 `TargetKbps` 与 `FrameRateExtN`——CBR 用帧率折算每帧预算，只改采集节奏不改编码器会让目标失真；新增 `framerate()` 访问器。
  - 主机：采集节奏由 `frame_duration(fps)` 计算，决策变化时同步重算 `next_frame_due`、置 `force_keyframe`、把 `stream_info.framerate` 一起写进 `refresh_stream`；重建编码器时控制器与阶梯一并重置。`HostServerStats::framerate` 进遥测，托盘与 `chast-quic` 输出同时报告。
  - 验证（2026-10-22，单机）：`chast-bitrate-test` 共 82 项断言全过（新增帧率阶梯的四档边界、12 fps 配置的下限、恢复时爬梯、预算扣除与夹取）；`chast-quic verify` 74 项断言全过（新增“查看者看到的帧率等于主机采集帧率”“干净链路保持配置帧率”），151 个访问单元；真实 `serve --audio` + `connect` 打印 `video budget 2601 kbps after reserving audio out of 2777 kbps` 与 `target-bitrate=2601 kbps framerate=30 abr-changes=0`；`ctest -C Release` 8/8。
  - 设计取舍：帧率是码率的函数而不是第二套状态机（不引入第二组冷却/恢复计时，避免两个调速器打架）；降帧率的目的不是省带宽而是省“每帧比特”（CBR 下总量不变），所以档位按比例分而不是按绝对帧率分；预算扣在上限侧而非目标侧，恢复逻辑因此天然收敛。
  - 已知限制：决策到下一帧之间最多有一个已排定的帧按旧节奏发出；音频预算按 `AudioInfo::bitrate` 固定扣除，不跟踪瞬时码率；真实弱网（丢包/抖动/带宽突变）仍需双机验收，本轮是合成样本 + 回环。
- 已完成：分辨率自适应 M6.1（`docs/m6.1-plan.md`）。
  - 问题：码率和帧率两个杠杆仍不够。1080p 在 25% 预算（约 700 kbps、15 fps）下每像素只有约 0.023 bit，文字糊成块；降低编码分辨率才能让像素数与可用比特重新匹配。
  - 阶梯：`BitrateController` 按目标/上限比例分三档（≥50% 100%、30–50% 75%、<30% 50%），带滞回（75%→100% 需 ≥55%，50%→75% 需 ≥34%）；50% 在 ≥55% 时直接回 100%，避免连续两次重建。`BitrateDecision` 每次携带 `resolution_percent`。
  - 零拷贝缩放：`IDxgiCapturer::set_nv12_scale()` 在原 BGRA→NV12 视频处理器上把目标矩形改小，图像不出显存、不换卡；BGRA 采集保持原尺寸，档位跨 `reinitialize()` 保留，重建失败回退上一档并触发全量重转。
  - 协议 v12：HelloAck/StreamInfo 追加 `source_width/source_height`；`width/height` 继续表示每个访问单元的编码尺寸。指针坐标、输入事件与光标精灵比例始终使用源尺寸，缩放不破坏 1:1 映射；解析器拒绝缺失或零源尺寸。
  - 主机：档位变化时先按当前 ABR 目标/帧率重建编码器，成功后再重公告；重建后连发两个 IDR，覆盖 StreamInfo 与视频帧的跨流竞争。任一环节失败则回退并固定本会话档位，避免每秒重试。
  - 查看器/Dart：`ViewerStatus` 分开 `width/height`（编码）与 `sourceWidth/sourceHeight`（桌面）；`RemoteView` 用编码尺寸定宽高比、用源尺寸做指针映射，光标比例改为“绘制宽度 / 源宽度”；浮层在缩放时显示源尺寸。
  - 验证（2026-10-22，单机回环）：`chast-bitrate-test` 新增档位边界、滞回、50→100 直达与恢复爬梯；`chast-protocol-test` 覆盖源尺寸往返与截断/零值拒绝；`chast-quic verify` 在 1920×1080 源上 2.5 s 缩放到 1440×810，121 个访问单元全部解码，CRC/解码/协议错误为 0，离线重放帧数与摘要一致；`ctest -C Release` 8/8、`flutter analyze`、`flutter test`、`flutter build windows --release` 全部通过。
  - 已知限制：每跨一档重建编码器，期间可能丢 1–2 帧；缩放使用视频处理器固定滤波，文字边缘比原生分辨率软；查看器内的指针 1:1 与缩放后文字可读性仍需带物理鼠标的人工确认；真机弱网档位轨迹待双机验收。
- 已完成：单机弱网验收（`docs/weak-network-acceptance.md`）。
  - 工具：新增 `chast-netem`（`tools/netem.cc`），在查看者与主机之间做 UDP 透明中继，按时间窗口注入单程时延、抖动、独立丢包和每方向带宽瓶颈；QUIC 拥塞控制、丢包检测与 ABR 接收的都是真实数据报信号。中继收发缓冲设为 16 MiB——默认 64 KiB 装不下切档后的双 IDR 突发，会在工具内静默丢包并伪造成链路拥塞。
  - 结果（单机，1920×1080，80 ms + 0–20 ms 抖动、3% 丢包、1000 kbps，劣化 20 s 后恢复）：档位 3000→500 kbps、30→10 fps、100%→50% 用时约 10.4 s，随后按 +100 kbps/3 s 单调回到 2700 kbps / 30 fps / 100%；1850/1850 帧解码，CRC/解码/协议/序号错误全为 0；干净对照 751/751 帧、`abr-changes=0`、最大帧龄 21.5 ms。
  - 待解决：容量 3× 阶跃下降时帧龄峰值约 8 s（15% 的帧晚于 1 s），根因是 ABR 阶梯收敛慢于可靠有序流的排队速度；链路恢复后约 1 s 排空。后续可选“按实测交付速率一次性降档”（不动协议，优先）、“发送端时延预算 + 跳帧 + 强制 IDR”或“视频改走 QUIC DATAGRAM”。
  - 已知限制：单机回环没有真实无线抖动、突发丢包、NAT/PMTU 与跨机时钟偏差；Flutter 查看器呈现路径与双机验收仍待做。

## M7 — 安全模型

- 实现与验收完成（2026-10-22）：永久/临时密码、TOTP、可信设备、失败锁定、
  IP 白名单、权限协商、主机安全设置/审批 UI、CLI 认证参数与查看器
  认证/权限 UI 均已落地，协议升到 v13。审批超时后的僵留对话框已修复：
  审批请求携带 `peer_alive`，查看者离开后传输层拒绝、`HostServer` 不提交
  安全状态、主机审批窗自动关闭。详见 `docs/m7-plan.md`、
  `docs/m7-status.md`。
  - 验证：`ctest --test-dir build -C Release` 10/10；`chast-quic verify`
    83 项断言全过（151 个访问单元，新增 2FA/可信设备/锁定/白名单回环）；
    CLI 正确/错误密码与 TOTP 端到端；真实审批窗在查看者超时后自动关闭；
    Flutter `analyze` 无问题、62 项测试通过、Windows Release 构建成功。
  - 待办：关闭一次未能复现的 COMCTL32 崩溃；完成查看器密码/2FA 对话框的
    人工 GUI 验收。

## M8 — 发射端加固（进行中）

计划与逐项记录见 `docs/m8-plan.md`。

- 已完成：麦克风与双向音频 M8.5（协议 v18）。
  - 编码统一走 vcpkg 的独立 Opus：系统声音 48 kHz 立体声 128 kbps，麦克风 48 kHz 单声道 32 kbps、20 ms/960 样本；`OpusHead` 携带 pre-skip。AAC 编码器与工厂已删除，`create_audio_encoder()` 只接受 Opus；`AudioCodec::AAC` 仅保留解码兼容，生产不产生 AAC 流。
  - 传输：查看器发起 `CMIC` 单向流（20 字节自描述头 + `OpusHead`），`kPermissionMicrophone` 由主机权限掩码控制，默认关闭；流打开/关闭就是麦克风开/关，权限收回会立即 abort。主机按流创建 Opus 解码器与 WASAPI 播放器，采用到达时间锚定 80 ms lead；查看器采集与编码在独立线程，帧循环队满只丢当前包，不阻塞视频。
  - 查看器：`ViewerEngine` 暴露授权/发送状态、设备、包数、丢弃与错误；原生桥提供 `setMicrophone/listMicrophones/selectMicrophone`；Flutter 工具栏麦克风按钮 + 设备菜单，状态浮层显示上行包数与丢弃。
  - 验证（2026-10-24，单机回环）：`chast-quic verify --microphone` 全部断言通过，`CMIC` 上行 90 包、主机解码 86088 帧、注入播放器测得 440 Hz/RMS 6864，关闭闸门的主机拒绝 `CMIC` 且视频继续；179 个访问单元、摘要 `CB960C0EAC53FDE2`。`ctest -C Release` 11/11；`flutter analyze`、68 项 `flutter test`、Windows Release 构建通过。
  - 已知限制：真实麦克风“打开即响、关闭/收回即停”与双机回声/延迟仍需实机；AEC、噪声抑制、唇音同步不在本轮。
- 已完成：锁屏/UAC 与隐私模式 M8.6（协议 v19）。
  - 主机每秒读取输入桌面并通过 `DesktopState` 公告：能打开 Winlogon 的服务态按设计继续采集与注入（待实机验证），托盘态打不开时暂停采集并在解锁后自动重建，不结束会话；注入线程同样跟随桌面，切换时清空按键/按钮持有状态。
  - 隐私模式：`kPermissionPrivacyMode`（默认关闭）控制 `SetPrivacyMode`，主机用 `WDA_EXCLUDEFROMCAPTURE` 的置顶穿透黑色遮罩遮挡本机屏幕，DXGI 采集不变；权限收回或会话结束立即撤罩。查看器提供盾牌开关、应答提示与安全桌面横幅。
  - 验证（2026-10-24，单机回环）：`chast-quic verify --privacy` 100 项断言全过，遮罩 1.32 s 开、3.54 s 关，期间视频持续且画面未变黑；关闭闸门的主机拒绝请求且不断流；181 个访问单元、摘要 `4DB1C6A97132DC4F`。`ctest -C Release` 12/12；`flutter analyze`、73 项测试、Windows Release 构建通过。
  - 已知限制：真实锁屏/UAC、服务态安全桌面采集与遮罩视觉仍需实机；虚拟显示器 M8.2 继续暂缓。
- 已完成：编码后端 FFmpeg 硬件兜底 M8.3（NVENC/AMF 实机验收待补；协议仍 v23，`docs/m8-plan.md` 第 9 节）。
  - 选择：`EncoderBackend::Auto` 仍先走 oneVPL 三级阶梯，全部失败后再创建 vcpkg FFmpeg 编码器（NVIDIA NVENC / AMD AMF / Intel QSV，按采集适配器厂商选，HEVC 可在同厂商退 H.264）；`Vpl`/`Ffmpeg` 固定后端失败不回退，工程不含软件编码。`HostServerConfig` 与工具新增 `--encoder auto|vpl|ffmpeg`。
  - 零拷贝：编码器自有 D3D11VA 帧池，采集 NV12 用同卡 `CopySubresourceRegion` 拷入；QSV 从采集设备派生并一次映射池帧，mfx-gen 要求设备开启 `ID3D10Multithread`，否则 SetHandle 返回 -16。`shared_import()` 对 FFmpeg 后端为 false（GPU 拷入而非共享导入）。
  - 速率：NVENC/AMF 纯码率变化原地更新、不打断编码序列，帧率变化延迟重建并强制 IDR；QSV 每次码率/帧率变化由 libavcodec reset 会话，要求一次 IDR，主机按 `keyframe_required` 执行。RustDesk 的 QSV（原生 mfx 与 ffmpeg_vram）同样以不带 reset option 的 reset 开始新序列，由硬件插入 IDR；其 NVENC/AMF 与我们一样不插入，其自适应帧率只改采集节奏、不写进编码器。一级 oneVPL 路径的纯码率变化用 `StartNewSequence=OFF`，不插 IDR。
  - 验证（2026-10-24，单机 QSV）：`chast-encode --encoder ffmpeg` H.264/HEVC 各 3 s 通过（52.6/48.7 fps、292/256 KB）；新增 `quic_loopback_ffmpeg` 与 `quic_stream_options_ffmpeg`，与原有 VPL 两条回环 4/4 通过；回放切分支持无 AUD 的 QSV 流（SEI+slice 按 VCL 边界成组）。
  - 复核（2026-10-25）：同步远端后重新 Release 编译，`ctest -C Release -R ffmpeg` 2/2 通过（33.75/31.74 s）；`chast-encode --encoder ffmpeg` 在 2560×1440 上 H.264 54.9 fps、HEVC 41.0 fps，`errors=0`、`import: copy`。
  - 已知限制：本机无 NVIDIA/AMD 显卡，NVENC/AMF 仅通过编译、厂商映射与可用性检查，真实编码与零拷贝待实机。

## M9 — 输入与连接补全（已完成）

计划与逐项记录见 `docs/m9-plan.md`。

- 已完成：Ctrl+Alt+Del、系统组合键与断线自动重连 M9.0（协议 v20）。
  - 协议：`SendSas = 28`（空载荷）与 `SasResult = 29`（`u8 code` + 原因），
    结果码 `Ok/Denied/Unsupported/Failed`；无输入权限不丢包，由主机明确拒绝。
  - 主机：`sas.dll` 运行时加载；只有服务态调用 `SendSAS`，托盘态回答
    `Unsupported`，测试可注入 `HostServerConfig::send_sas` 回调；失败只回传
    查看器，不结束会话；统计新增 `sas_requests/sas_failures`。
  - 查看器：特殊按键菜单（Win、Alt+Tab、Alt+F4、Ctrl+Shift+Esc）复用现有
    `InputEvent`，按下再抬起、每条携带修饰键位图；Ctrl+Alt+Del 走独立请求，
    状态新增 `sasCode/sasError/sasUpdates` 并按计数弹一次结果提示。
  - 自动重连：只在已出过首帧后 `state == failed`、无接入拒绝、无指纹变化时
    触发，退避 1/2/4/8/15 s、最多 5 次；复用隧道/密码/显示器选择，等待卡片
    显示第 n/5 次并可取消；首帧、手动重连和显示器/分辨率切换都会清零计数。
  - 验证（2026-10-24，单机回环）：`chast-protocol-test` 覆盖 SAS 两种消息；
    `chast-quic verify --sas` 96 项断言全过（主回环 1.62 s 回答 `Ok`、主机
    仅一次请求零失败；无回调主机回答 `Unsupported` 且视频继续），131 个访问
    单元、摘要 `C895EAB372C09B59`；`ctest -C Release` 12/12；
    `flutter analyze` 无问题、85 项测试通过、Windows Release 构建成功。
  - 已知限制：真实 `SendSAS` 的安全桌面效果、托盘态提示文案与真实断网重连
    节奏仍待人工/双机验收；单机只能验证请求/应答通道与退避策略。
- 已完成：查看器画质、帧率与编码器偏好 M9.1（协议 v21）。
  - 协议：`SetStreamOptions = 30`（u8 编码器偏好、u16 帧率上限、u8 画质档）
    与 `StreamOptionsResult = 31`（结果码 + 生效编码器/帧率/画质 + 原因），
    结果码 `Ok/Unsupported/Failed/Busy`；0 fps 表示跟随主机，未知枚举、
    越界帧率与尾随字节由解析器拒绝。
  - 主机：画质在“总预算减音频预留”的视频预算内取 100/75/50%，帧率上限取
    `min(主机配置, 查看器上限)`，两者经 ABR 与 `set_rate` 生效并重发
    StreamInfo；编码器切换先建替换实例、校验实际编码族再替换，失败时旧流
    不动，1 s 内重复切换回答 `Busy`；统计新增请求/生效/失败计数。
  - 编码器：H.264 的纯码率变化走
    `mfxExtEncoderResetOption{StartNewSequence=OFF}` 的连续 reset，参考帧
    保留、不插 IDR（初始化关闭 HRD/VUI 计时，SPS 不随码率变化）；帧率变化、
    首帧前与编码器替换仍要求 IDR，主机只按 `set_rate` 的
    `keyframe_required` 决定是否强制关键帧。mfx-gen 的 HEVC VDEnc 拒绝
    `MFXVideoENCODE_Reset`（连原样参数也返回 -14），`MfxEncoder` 改为同一
    VPL 会话关闭/重开兜底、失败恢复旧码率；顺带修复 HEVC 主机启动音频预留
    与 HEVC ABR 一直静默失败的问题。
  - 查看器：工具栏“画面”菜单（画质/帧率上限/编码器），选择立即下发并按
    结果计数确认或回滚，拒绝时用 SnackBar 显示主机原因；状态新增结果码、
    原因、计数与生效画质档；重连后重新下发同一组偏好。
  - 验证（2026-10-24，单机回环）：`chast-quic verify --stream-options`
    H.264 基线 295 个访问单元（仅画质 2636↔1977↔2636 kbps 往返时关键帧
    零增长）、HEVC 基线 294 个访问单元（重开产生 1 个关键帧），全部断言
    通过（含真实 HEVC 产出与回切）；`chast-encode --bitrate-switch` 在
    H.264 上确认 `keyframes=1`、HEVC 上 `keyframes=2`；`ctest -C Release`
    13/13；`flutter analyze` 无问题、91 项测试、Windows Release 构建通过。
  - 已知限制：无 HEVC 硬件时的拒绝文案、双机 HEVC 解码兼容性与不同网络
    下的画质体感仍待人工/双机验收；拒绝 Reset 的运行库上每次改码率都会有
    一次编码器重开。
- 已完成：多文件与文件夹传输 M9.2（协议 v22）。
  - 协议：`FileOffer = 21` 末尾追加可选的 `u16 relative_len` + UTF-8 相对目录
    （不含文件名，空表示接收根目录）；长度 ≤1024 B、≤32 段、只允许 `/`，
    拒绝绝对路径、盘符、`..`、`.`、空段和反斜杠；没有该字段的旧载荷按
    根目录解析。
  - 主机：相对目录逐段消毒后从接收根目录逐级创建，已存在的段必须是普通
    目录且不是 reparse point；叶子文件沿用唯一名、`.part` sidecar 与摘要
    校验，断点续传位置随目录变化；并发 offer 仍回答 `Busy`。
  - 查看器：`SendFiles` 一次接收多个路径/文件夹；常驻工作线程递归展开
    （跳过符号链接、设备、不可读项，空目录记一条失败）并逐个计算 SHA-256；
    会话线程按顺序 offer、发送、收尾，单个失败不阻塞队列，权限撤销清空
    整队；队列上限 10000 条。
  - UI：选择器加 `OFN_ALLOWMULTISELECT | OFN_EXPLORER`，`WM_DROPFILES`
    取回完整路径列表；进度覆盖层显示“相对目录/文件名 + 队列剩余 N 个”，
    完成后用 `lastFileName`/`lastFileError` 弹一次结果提示。
  - 验证（2026-10-24，单机回环）：`chast-protocol-test` 338 项、
    `chast-file-transfer-test` 50 项断言全过；`ctest -C Release` 13/13；
    `chast-quic verify --file-upload` 105 项断言、五个顺序 offer（含嵌套
    目录）全部通过，传输 964 KiB、恰好 3 个完整文件落盘，嵌套文件位于
    `<接收目录>/chast-verify/nested/`，本轮 181 个访问单元、摘要
    `7F2D789FFC5DFFC4`；`flutter analyze` 无问题、92 项测试、Windows
    Release 构建通过。
  - 已知限制：查看器的递归展开尚无自动化断言，真实多选对话框、资源管理器
    拖放和 GUI 队列/撤销体验仍待人工验收；不做并行传输、空目录和符号链接
    跟随；主机 → 查看器下载仍为单文件。
- 已完成：多显示器合并视图 M9.3（协议 v23）。
  - 协议：Hello 的 `display` 接受 `kDisplayAll = 0xFFFE`；HelloAck/StreamInfo
    flags 新增 `kServerInfoMergedLayout = 0x0040`（非权限位，权限掩码相等校验
    改用 `kServerInfoPermissionFlags`）；`ServerInfo.merged` 由 flag 解码；
    合并流要求成员列表非空、每个成员矩形都在并集内且并集等于原点/源尺寸
    （允许 1 像素偶数对齐）。
  - 主机：新增 `MergedCapturer`，在同一 D3D11 设备/上下文上为每块屏建子采集，
    按虚拟桌面并集用 `CopySubresourceRegion` 合成 BGRA8 画布，只拷贝产生新帧
    的显示器区域；画布统一做一次 NV12 转换，长边 >4096 或面积 >4096×2304 时
    自动等比缩小；跨适配器明确拒绝，光标按并集坐标映射并取所在屏形状。会话
    支持 Hello/会话内选 `kDisplayAll`、成员热插拔重建、合并空操作，合并流拒绝
    单屏模式切换；ABR 把请求比例与实际 NV12 比例分开，避免重复决策。
  - 查看器：`ViewerStatus.mergedLayout`、`kViewerDisplayAll`；显示器菜单新增
    “全部显示器（合并）”并显示并集尺寸/成员数，合并流高亮该条目、隐藏分辨率
    菜单；bridge 与 Start/SwitchDisplay 接受 `kDisplayAll`。
  - 验证（2026-10-24，单机回环）：`chast-protocol-test` 348 项断言（含合并
    HelloAck 往返、并集校验、空成员拒绝、权限位共存）；`chast-quic verify
    --merged` 91 项断言全过，单屏 1920×1080 合成并 100% 编码，切
    `kDisplayAll` 为空操作、越界索引被拒，150 个访问单元、摘要
    `CD1D7253759BFE8D`；`ctest -C Release` 14/14；`flutter analyze` 无问题、
    96 项测试、Windows Release 构建通过。
  - 已知限制：真实多屏布局（负坐标、横竖混排、不同分辨率/DPI）、跨适配器
    拒绝文案、热插拔重建与光标形状切换仍待多屏机器人工验收；不做每屏独立
    流、跨适配器 CPU 合成、合并下的单屏模式设置和合并选择持久化。
- 已完成：音频设备选择、回声消除与降噪 M9.4（协议仍 v23）。
  - 音频参考：`AudioReference` 为固定 48 kHz 立体声环缓冲（约 2 s），播放
    开始置 active、静音/停止置 inactive 并清零历史；主机以正在播放的远端
    麦克风 PCM、查看器以解码后的系统声音为 AEC 参考，读取时固定回退 120 ms。
  - DSP：`chast.audio` 引入 SpeexDSP，`AudioPreprocessor` 按 10 ms/480 样本
    帧做 AEC（近端 1/2 声道、参考 2 声道、200 ms 滤波器），可选 DENOISE，
    AGC/VAD 保持关闭；参考 inactive 时严格旁路并重置滤波器；统计处理帧、
    欠载与重置次数，处理失败按原样通过。
  - 设备：主机设置新增 `audio_device`（留空跟随默认）与 `audio_aec`（默认
    开），同一 render 端点用于系统声音捕获与远端麦克风播放，托盘“高级”页可
    选择并刷新；查看器音量按钮长按列出输出端点，会话内切换只重建播放器并
    保留解码器与时钟偏移，固定端点失效时回退默认并在 `audioError` 说明；
    查看器麦克风菜单新增 AEC/NS 开关，改动重建采集与 `CMIC` 流。
  - 状态/UI：`audioDevice/audioDeviceName/audioAec/audioNoiseSuppression` 与
    麦克风 `aecFrames/nsFrames` 贯通 bridge、Flutter 状态与提示。
  - 验证（2026-10-24，单机回环）：新增 `chast-audio-dsp-test`，合成回声
    ERLE 28.31 dB、双讲相关性 0.994、降噪 14.27 dB、谐波保留 85.04%，
    严格旁路与欠载计数通过；`ctest -C Release` 15/15；普通回环 121 个访问
    单元、摘要 `38EBB31B754CB099`，`verify --microphone` `CMIC` 96 包/
    91848 帧、439 Hz、151 个访问单元、摘要 `22874510645B8DAB`；
    `flutter analyze` 无问题、104 项测试、Windows Release 构建通过。
  - 已知限制：真实扬声器-麦克风回路的 AEC 收敛与听感、设备热插拔、双机
    外放回声仍需人工验收；不做 AGC、端点自动切换与虚拟声卡。

## M10 — 传输回退（已完成）

- 已完成：TCP/WebSocket 弱网回退 M10.0（协议仍 v23，`docs/m10.0-plan.md`）。
  - 载体：QUIC 数据报原样装入 TCP 帧或 WebSocket binary message；主机在 QUIC
    端口 P 旁监听 `P+1`，同一端口按首字节区分原始 TCP 与 WS，10 s 握手、
    64 KiB 帧上限、16 连接上限、180 s 空闲关闭；每个连接一个本地 UDP 套接字
    与一条 QUIC 连接，TLS/指纹/令牌/审批/权限仍端到端。
  - 查看器：`ViewerEngine` 的 `transport=auto` 依次尝试 QUIC 4 s、TCP、WS，
    `quic`/`tcp`/`ws` 可强制；pin 失败为终态；载体桥在 127.0.0.1:0 暴露 UDP，
    Flutter 从隧道 `path` 事件的 relay 地址或直连 `host:P+1` 取端点；状态新增
    `transport`/`transportError`，浮层显示 UDP/TCP 回退/WebSocket 回退。
  - 服务端：每个中继端口同时监听 TCP；主机代理建立持久认证链路，服务端配对
    单个查看者，忙则拒绝，链路断开清理；统计新增 `tcp_to_host_bytes`、
    `tcp_to_viewer_bytes`、`tcp_viewers`、`tcp_busy`。TCP 绑定失败只禁用该
    端口的载体，UDP 中继继续。（M10.1 起主机链路改为 `CHASTT-HOST2` +
    Ed25519 一次性挑战，不再使用 `key_hash`。）
  - 主机：`host.ini` 新增 `carrier=0/1`（默认开），载体监听失败不阻止 QUIC；
    托盘防火墙/服务安装同时放行 UDP 与载体 TCP；载体本地 UDP 缓冲 4 MiB，
    避免关键帧突发被系统默认缓冲丢弃而误触 ABR。
  - 工具：`chast-quic connect/verify --transport udp|tcp|ws [--carrier host:port]`
    （`quic` 为 `udp` 别名），回环断言包含 `transport`。
  - 验证（2026-10-24，单机回环）：`chast-carrier-test` 57 项断言；`ctest
    -C Release` 18/18（150.38 s，含 `quic_carrier_tcp`/`quic_carrier_ws`）；
    UDP/TCP/WS 三条基础回环各 151 个访问单元、摘要 `25CE9321F21BF8CC` /
    `EA76FC9E972BB17C` / `A8685816B1D13F0D`；TCP/WS 再以文件上传/下载与隐私
    的完整探针复跑，各 181 个访问单元、摘要 `CE8392D146C2570A` /
    `D784E6732DFF6B2C`，CRC/解码/序号错误为 0；`go vet`/`go build`/`go test
    ./...` 通过；`flutter analyze` 无问题、106 项测试、Windows Release 构建
    通过。
  - 已知限制：真实 UDP 封锁→自动回退、经服务中继的 C++ 主机 + Flutter 全链路、
    WebSocket 经过真实 HTTP 代理、以及双机弱网吞吐/延迟仍需人工验收；载体不做
    TCP 打洞、不做多查看者，不改变协议版本。

## M10.1 — 服务端向 RustDesk 对齐（已完成）

- 已完成：持久身份、挑战应答、候选遥测与共享载体（协议仍 v23，
  `docs/m10.1-plan.md`）。
  - 身份：ID 与 Ed25519 公钥永久绑定（`chast-register-v1` 签名 + 时间戳），
    `chast-server-state.json` 持久化，离线查询仍返回身份与服务器签名的
    `CHASTB1.<payload>.<sig>` 绑定；`/v1/server` 公布服务器公钥，
    `DELETE /v1/identities/{id}` 是丢失密钥的人工出口。
  - 挑战：keepalive、UDP 中继、TCP/WS 载体分别使用 30 s 单次 nonce，
    签名覆盖 ID、端口与观察地址；服务端只保存公钥。
  - 候选：主机上报 `local_addrs`，代理用会合/中继两处观察映射区分
    `asymmetric`/`symmetric` NAT；查询新增 `candidates`、`nat_type`。
  - 共享载体：`chast-server --carrier-listen`（默认 0.0.0.0:8445）一个端口
    承载所有主机链路（`CHASTT-HOST2` + 挑战）；原始 TCP 查看者用
    `CHASTT-V2` + `{host_id,token}`，WS 查看者 `GET /chast`（子协议
    `chast-carrier.v1`）以 binary hello 选择主机。令牌只在主机在线时出现在
    查询响应，租约失效后轮换；忙、坏令牌与超时都会拒绝。主机代理优先用
    注册响应里的 `carrier`，否则回退到中继端口 TCP；Flutter/工具从隧道
    `path` 事件取 `carrier`/`carrier_token`。
  - 统计：`/v1/stats` 的 `relay.shared_carrier` 暴露 `hosts`、`host_links`、
    `viewers`、`busy`、`rejected`、`to_host_bytes`、`to_viewer_bytes`。
  - 验证（2026-10-24，单机回环）：`go vet`/`go build`/`go test ./...` 通过；
    `chast-carrier-test` PASS（共享 hello 魔数、host id + token、缺 token 拒绝）；
    `ctest -C Release` 20/20（212.98 s）；共享载体 TCP 回环 692 帧、摘要
    `B2A69C6EA526D137`，WS 回环 692 帧、摘要 `EB0FC78F17BE208E`，CRC/解码/
    序号错误均为 0；错误令牌被拒绝，`shared_carrier` 统计符合预期；
    `flutter analyze` 无问题、`flutter test` 106 项通过。
  - 已知限制：真实 NAT/IPv6 跨网分类、共享载体经过真实 HTTP 代理需要双机或
    部署环境复核；共享载体仍为单查看者；服务端 HTTP 明文与限流由 M10.2 补齐。

## M10.2 — 服务端运维能力（已完成）

- 已完成：原生 TLS、HTTP 限流与 Prometheus 指标（协议仍 v23，
  `docs/m10.2-plan.md`）。
  - TLS：`--tls-cert`/`--tls-key` 同时给出即 HTTPS 监听，最低 TLS 1.2，
    启动时加载证书；`/v1/server` 增加 `tls` 字段；不给证书保持明文以兼容
    反向代理部署。
  - 限流：`internal/ratelimit` 按客户端 IP 令牌桶，`--api-rate`（默认 50/s）
    与 `--api-burst`（默认 100），`0` 关闭；覆盖全部 HTTP 路径，超限
    `429` + `Retry-After: 1`；`--proxy-header X-Forwarded-For` 取可信代理的
    最后一跳。
  - 指标：`GET /metrics` 输出 Prometheus 文本，带 server-key 时需要 Bearer；
    含主机/身份数、UDP/TCP/共享载体流量与忙拒绝、HTTP 请求数/在途/限流数/
    延迟汇总；标签不含主机 ID 或客户端地址。
  - 验证（2026-10-25，单机回环）：`go vet`/`go build`/`go test ./...` 通过；
    限流突发放行后 429 并在补充后恢复；HTTPS 自签证书下 `/healthz` 200、
    `/v1/server` 的 `tls:true`、TLS 1.2/1.3 均可协商且证书校验通过，明文
    请求被 400 拒绝；`/metrics` 未认证 401、带 key 后内容与 `/v1/stats`
    一致。
  - 已知限制：证书轮换/ACME/mTLS 不在范围内；限流基于内存桶，重启后清零，
    多实例部署需要前置网关统一限流；真实反向代理头与多客户端压力需部署
    环境复核。

## M10.3 — 载体在线、强制中继与查看者回退（已完成）

- 已完成：共享载体链路计入在线、`--relay-only` 与 Go 查看者载体泵
  （协议仍 v23，`docs/m10.3-plan.md`）。
  - 在线：`relay.Manager.HostLinked` 暴露已认证的主机载体链路；HTTP 查询在
    UDP keepalive 失效时仍返回 `online:true`，并隐藏过期 UDP 候选。
  - 查询：新增 `relay_only` 与 `transport`（`direct`/`relay`/`carrier`）；
    `--relay-only` 隐藏全部直连候选、固定 `nat_type:"symmetric"`，
    `/v1/server`、`/v1/stats` 与 `/metrics`（`chast_server_relay_only`）同步
    报告。
  - 查看者：`chast-tunnel viewer --mode carrier` 使用 `CHASTT-V2` 共享载体，
    本地 UDP 数据报 ↔ `u32le` 帧；断链 0.5 s→5 s 退避重连，30 s 空帧保活，
    服务器以空帧应答；`auto` 按 `transport` 选路，`direct` 在强制中继下报错。
  - 验证（2026-10-25，单机回环）：`go vet`/`go build`/`go test ./...` 通过；
    强制中继服务器下查询无候选、`nat_type=symmetric`，`transport` 分别为
    `carrier`/`relay`，载体与 UDP 中继均完成数据报往返，`direct` 明确失败；
    `--offline-after 1s` + 5 s keepalive 时 `last_seen_ms` 已过期但查询仍经
    载体返回在线，往返成功。
  - 已知限制：载体仍为单查看者，不承载多路复用；主机关闭后要等载体读超时
    （90 s）或 UDP 判定过期才转为离线；真实弱网重连与双机 UDP 封锁需人工
    验收。

## M10.4 — 管理通道、打洞审计与注册防滥用（已完成）

- 已完成：注册阻断、打洞请求环形日志、运行时强制中继与回环管理通道
  （协议仍 v23，`docs/m10.4-plan.md`）。
  - 注册阻断：`internal/abuse` 在 server-key、JSON 与 ID 校验之后、公钥与
    签名验证之前按观察 IP 计数；`--register-max`（默认 30）次 /
    `--register-window`（默认 60 s）超过返回 `429` + `Retry-After: 1`；
    `--register-max-ids`（默认 300）个不同 ID / `--register-id-window`
    （默认 24 h）超过后只拒绝新 ID，已见 ID 仍可续约；条目上限 65536，
    超出淘汰窗口最旧；两个上限都为 0 时不创建 Blocker。
  - 打洞审计：`internal/punchlog` 环形缓冲只在目标在线、会合即将回复双方时
    记录 `time/from_ip/to_ip/id`；同一 `from_ip + to_id` 60 s 内去重且只扫描
    最近 30 条；`--punch-log-max`（默认 100000，`0` 关闭）写满覆盖最旧。
  - 运行时强制中继：`api.Config.RelayOnly` 改为 `*atomic.Bool`，`lookup`、
    `/v1/server`、`/v1/stats` 与 `/metrics` 都读取当前值；管理通道
    `aur Y|N` 立即生效，已建立的查询与链路不受影响。
  - 管理通道：`--admin-listen 127.0.0.1:8446`（默认，空值禁用），启动时拒绝
    非回环绑定，连接来源不是回环直接关闭；一行文本命令（≤1 KiB），处理完
    写回纯文本并关闭。命令 `h`、`stats`、`hosts [count]`、
    `punch-requests|pr [start] [count]`/`pr -`、
    `ip-blocker|ib [ip]`/`ib -`、`relay-only|aur [Y|N]`；未知命令返回单行
    `unknown command`。
  - 统计：`/v1/stats` 新增 `registration_blocked`（`tracked_ips`、
    `rate_blocked`、`id_blocked`）与 `punch_requests`（`total`、`deduped`、
    `stored`）；`/metrics` 另含 `chast_punch_requests_total`、
    `chast_punch_requests_deduped_total`、`chast_punch_log_entries`、
    `chast_register_blocked_total{reason}` 与 `chast_registration_tracked_ips`。
  - 验证（2026-10-25，单机回环）：`go vet ./...`、`go build ./...`、
    `go test ./... -count=1` 通过；管理通道 `stats`/`hosts` 显示 `devbox`
    在线与候选 `127.0.0.1:19434`，`pr` 出现查看者打洞
    `127.0.0.1 -> devbox@127.0.0.1`，`pr 0 1` 分页与 `pr -` 清空均正常；
    `regflood -n 4` 得到 `200,200,200,429`，`ib 127.0.0.1` 显示
    `count=4 ids=3`、`rate_blocked=1`，`/v1/stats` 与 `/metrics` 计数一致；
    `aur Y` 后新查询 `relay_only:true`、`transport:"relay"` 且无候选，
    `aur N` 恢复直连候选；`--admin-listen 0.0.0.0:18456` 以
    `must be a loopback address` 退出。
  - 已知限制：管理通道为回环明文，不做远程鉴权传输；计数在内存中，重启后
    清零，多实例部署需要前置统一限流；未实现 hbbs 的 `ip-changes` 跟踪与
    `get_relay_server` 多中继轮换，后者仍是下一阶段候选。

## M10.5 — IP 变更审计与中继地址轮换（已完成）

- 已完成：hbbs 风格的 IP 变更审计与多中继地址轮换（协议仍 v23，
  `docs/m10.5-plan.md`）。
  - `internal/ipchange`：认证后的 HTTP 注册与 UDP keepalive 共享一个
    Tracker；首次只建基线，180 s 窗口内变化记录上一个与新 IP，360 s 后
    剪枝；单 IP 条目只在输出层过滤，自动指标抓取不会清空审计；基线与条目
    各上限 65536。
  - `internal/relayaddr`：无端口的主机名/IP 列表，CSV 设置、去重、`rs -`
    恢复；注册按轮换分配，已有租约保持地址；响应新增 `relays`（列表每个
    地址拼接该主机的租约端口），`relay`/`carrier`/查询/keepaliveAck 使用
    租约地址；新增 `--relay-servers`。
  - 中继源地址：注册时把该端口的 UDP/TCP 监听重绑到租约地址
    （`relay.Manager.BindHost`），多 IP 主机上查看者访问哪个地址，回包就
    从哪个地址发出；地址不是本机或无法解析时回退 `--relay-listen`。
  - 管理通道新增 `rs`/`rs a,b`/`rs -`、`ic`/`ic <id>`/`ic <start> [count]`/
    `ic <id> -`/`ic -`、`tg <ip1> [ip2]`；`stats` 增加
    `ip_changes`/`relay_servers`，`hosts` 增加 `relay_host`。
  - 统计：`/v1/stats` 增加 `ip_changes`（`tracked`/`changes`）与
    `relay_servers`；`/metrics` 增加 `chast_ip_change_events_total`、
    `chast_ip_change_tracked` 与 `chast_relay_servers`。
  - 验证（2026-10-25，单机回环）：devbox/devbox2 分别得到
    `127.0.0.1:41100`/`127.0.0.2:41101`，两者的 `relays` 都含两个端点，
    中继查看者双向回显成功；`netstat` 确认端口绑定到各自租约地址。devbox
    从 `127.0.0.4` 重启后 `ic` 显示 `.1/.2/.4` 且中继地址不变。`rs` 改为
    仅 `.2` 后新主机 devbox3 得到 `127.0.0.2:41102`、既有租约不变，`rs -`
    恢复；`tg` 预览与下一次分配一致；`ic` 分页/删除/清空与指标计数一致。
    `go vet ./...`、`go build ./...`、`go test ./... -count=1` 全部通过。
  - 已知限制：列表条目必须指向同一个 chast-server 实例；不做
    `check_relay_servers` 健康探测与跨服务器中继；`tg` 不读取 GeoIP；
    地址变化时端口会短暂关闭并重绑，现有查看者依赖 QUIC 重传恢复。

## M10.6 — 独立中继节点与健康探测（已完成）

- 已完成：hbbr 式的独立中继进程与 hbbs 侧节点池（协议仍 v23，
  `docs/m10.6-plan.md`）。
  - `cmd/chast-relay`：控制面 `POST /v1/leases`、`DELETE /v1/leases/{id}`、
    公开 `GET /healthz`、Bearer `GET /v1/stats`；UDP/TCP 中继端口集合、
    可选共享载体、租约清理协程与 `--advertise` 重绑；节点只保存 hbbs
    下发的公钥与载体令牌，`relay_hello` 与载体挑战在节点本地验证。
  - `internal/relaypool`：URL 规范化（仅 http/https、无路径/凭据）、
    周期性 `/healthz` 探测、RTT/租约/载体在线快照、健康节点轮询、
    首选节点粘性申请、`Preview`、best-effort 异步释放。
  - 注册流程：优先向主机上次的节点续租；失败时按轮询尝试其余健康节点；
    全部不可用则回退本机中继并异步释放旧租约。切换节点时异步释放旧租约，
    本机回退只在原租约属于本机时沿用原 `relay_host`。
  - 查询与管理：`relay`/`carrier`/`transport`/`online` 按租约来源返回；
    `stats` 增加节点计数；`hosts` 增加 `node=` 并按节点缓存判定 carrier；
    `rs` 输出节点状态，`tg` 优先预览下一个健康节点；`/v1/stats` 增加
    `relay_nodes`，`/metrics` 增加 `chast_relay_nodes`、
    `chast_relay_nodes_healthy`。
  - 验证（2026-10-25，单机回环）：远程租约 `127.0.0.1:41100`、
    载体 `127.0.0.1:18448`，relay/carrier 两条路径的 UDP 回显均成功；
    停止节点后 `rs` 转为 down、主机续租回退本机 `41200` 且回显成功；
    重启节点后续租迁回 `41100` 并释放本机端口；节点 `/healthz` 的
    `leases`/`linked`、hbbs 管理台与指标一致。`gofmt -l .`、`go vet ./...`、
    `go build ./...`、`go test ./... -count=1` 全部通过。
  - 已知限制：已有查看者不随节点故障迁移；健康快照最多滞后一个探测周期；
    控制面默认明文；跨机时钟偏差会影响租约到期。

## M10.7 — 中继带宽限制、黑名单与降级（已完成）

- 已完成：hbbr 式的中继资源治理（协议仍 v23，`docs/m10.7-plan.md`）。
  - `internal/relay/policy.go`：单会话/总带宽令牌桶、黑名单慢速档、阻断名单、
    自动降级与活动会话用量；UDP 超限整包丢弃，TCP/WS 载体等待形成背压。
  - 会话键：`udp:<port>`、`carrier:<port>`、`shared:<hostID>`；双向流量计入
    同一会话，名单按查看者地址匹配，每 1 s 复核一次，阻断时立即释放 UDP
    查看者槽位并关闭载体会话。
  - 控制台与 hbbr 同名：`ba/br/b`、`Ba/Br/B`、`sb/tb/ls`、`dt/t`、`u`；
    hbss 管理台与 `chast-relay` 回环控制台共用 `Policy.Command`。名单文件
    可选，显式路径下启动加载、增删后原子回写。
  - `cmd/chast-relay` 新增 `--admin-listen`（默认 `127.0.0.1:8449`）与全套
    策略参数；`cmd/chast-server` 新增同一组策略参数。
  - `/v1/stats` 增加 `relay.policy` 与 `relay.sessions`；`/metrics` 增加
    `chast_relay_single_bandwidth_bps`、`chast_relay_total_bandwidth_bps`、
    `chast_relay_blacklist_speed_bps`、`chast_relay_policy_blacklist`、
    `chast_relay_policy_blocklist`、`chast_relay_policy_sessions`、
    `chast_relay_limited_bytes_total`、
    `chast_relay_rate_dropped_total{unit}`、
    `chast_relay_blocked_sessions_total`、
    `chast_relay_downgraded_sessions_total` 与
    `chast_carrier_blocked_total`。
  - 验证（2026-10-25，单机回环）：节点 `0.08/100/0.05` Mbit/s、hbss
    `64/512/16` Mbit/s，控制台与指标一致；阻断前探针无回包、解除后 3 次往返
    成功；burst 400 × 1400 B 只收到 4 个回包，节点计数 `limited_bytes=319200`、
    `rate_dropped_datagrams=228`、`downgraded_sessions=1`，`u` 为
    `downgraded=true`；运行时 `ba`/`br` 即时改变黑名单状态；名单文件启动加载；
    节点故障回退本机中继后阻断/限速/降级计数同样生效。`gofmt -l .`、`go vet
    ./...`、`go build ./...`、`go test ./... -count=1` 全部通过。
  - 已知限制：只按会话与全局两级限速，不支持按 ID；UDP 只能整包丢弃；名单
    不与节点共享；降级只降不升；真实跨机整形效果与时钟偏差下的降级判定仍需
    部署复核。

## M10.8 — 配置来源、UDP 接收缓冲与启动自检（已完成）

- 已完成：hbbs/hbbr 式的运维入口（协议仍 v23，`docs/m10.8-plan.md`）。
  - 配置来源：`internal/servercfg` 提供「显式 flag > `--config` INI > `--env-file`
    dotenv > 环境变量」四级来源；键名大小写不敏感，`-`/`_` 等价，接受
    `CHAST_<KEY>` 或 `<KEY>`；INI 跳过分节与注释，dotenv 支持 `export ` 与
    成对引号；布尔值额外接受 `Y/N`。`cmd/chast-server` 与 `cmd/chast-relay`
    新增 `--config`、`--env-file`，在读取任何业务参数之前应用。RustDesk 的
    `BIND`/`PORT`、`RELAY-SERVERS`、`RMEM`、`TEST_HBBS`、`ALWAYS_USE_RELAY`、
    `MAX_PUNCH_REQUESTS` 作为别名可用，同层 canonical 键优先。
  - 接收缓冲：`--rmem` 为会合套接字和全部中继 UDP 端口（含按租约地址重绑）
    设置并回读 `SO_RCVBUF`，失败只告警；`/v1/server`、`/v1/stats` 与
    `/metrics` 暴露生效值。共享载体只有 TCP/WebSocket，没有 UDP 侧。
  - 默认名单与自检：名单路径留空时自动加载工作目录 `blacklist.txt`/
    `blocklist.txt` 并记录实际路径；`--test-punch`（`TEST_HBBS`）支持空值跳过、
    `auto` 本机探测和显式 `host:port`，注册临时 ID 后每秒重试，12 s 无应答
    失败退出，成功记录 RTT。
  - 验证：单机回环覆盖显式 flag > INI > .env > 环境变量、1 MiB 接收缓冲在
    `/v1/server`/`/v1/stats`/`/metrics` 一致、默认名单在控制台 `b`/`B` 可见、
    自检成功（RTT 2 ms）与关闭端口 12 s 失败（exit=1）；`go vet`/`go build`/
    `go test ./... -count=1` 通过。
  - 已知限制：配置只在启动时读取，不支持热重载；INI 无分节层级；dotenv 不
    展开变量；`--test-punch` 仅 `chast-server` 提供；`DB_URL`/SQLite 持久化
    不在范围内（状态仍用 JSON 文件）。

## M6 后续（未开始）

- 音频质量调优：唇音同步校准、设备热插拔自动切换（AEC/降噪/设备选择已在
  M9.4 完成）。
- 更多采集/编码后端与其他平台。
- M8 其余项（虚拟显示器、安装器与 i18n）见 `docs/m8-plan.md`。

## 推进原则

1. 每一阶段先补可重复的回环验证，再改 UI。
2. 协议字段只增不改；版本号随语义变化提升，ALPN 家族名不变。
3. 输入默认关闭，闸门在服务端；客户端不得绕过 `HelloAck` 的标志。
4. 键鼠事件逐条发送、按序应用；宁可丢新事件，也不阻塞视频路径。
