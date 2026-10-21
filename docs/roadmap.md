# chast 开发路线图

更新时间：2026-10-21。按依赖顺序排列，已完成项保留在这里作为基线。

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
  - 已知限制：身份属于当前 Windows 用户，服务账户（M3.2 的 Windows 服务）需要自己的密钥与证书；指纹校验不做证书链或有效期检查（自签名场景下无意义）。
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
  - 已知限制：托盘只服务当前登录用户，锁屏/未登录时不工作（Windows 服务属于 M3.2b）；身份证书在用户证书库，服务账户需要自己的证书；托盘没有单实例跨会话（不同用户各一份）；界面为固定布局，只按 DPI 缩放，未做自适应重排。
- 待做：Windows 服务（M3.2b）：`%ProgramData%` 机器级设置、服务账户身份、服务模式防火墙与安装/卸载。
- 若要在不可信网络使用，M3.1 必须排在 M4 之前。

## M4 — 连接与地址簿

- 已完成：Go 服务端与隧道代理 M4.0（`server/`）。
  - 服务端 `chast-server` 提供三件事：HTTP 注册表（`POST/GET/DELETE /v1/hosts`、`/healthz`、`/v1/stats`）、UDP 会合端口和按主机分配的 UDP 中继端口。参考 RustDesk `hbbs`/`hbbr` 的信号流程，但没有照搬“按 uuid 配对两条 TCP 流”：chast 的传输是 MsQuic QUIC，TCP 中继必须终止 QUIC 或改传输层，所以这里改为**透明转发 UDP 数据报**，每个主机一个中继端口，查看者把该端口当作普通 QUIC 服务端地址，TLS、令牌、审批和指纹 pin 仍然只在两端生效，服务器无法解密。
  - NAT 穿透由两侧 `chast-tunnel` 代理完成：MsQuic 不暴露内部 UDP 套接字，代理各自持有一个对外数据套接字，用它保活、打洞并转发真实数据。服务器只根据会合端口观察到的地址交换候选，不信任客户端自报；双方从同一套接字互发探针，锥形 NAT 可直连，对称 NAT 则失败。查看者 `auto` 模式在 `--direct-timeout` 后切到中继并保持，`direct`/`relay` 用于强制验证单条路径；因为切换前后代理的五元组不变，QUIC 不需要重连逻辑。直连时数据不经过服务器。
  - 中继端口沿用 `HostServer`“同一时刻一个查看者”的语义：活跃查看者存在时其他来源收到 `busy`，空闲 60 s 或主机 90 s 无流量即回收；主机必须先用密钥摘要通过 `relay_hello` 绑定端口，未认证来源不能改绑。会合端口对 `punch_request` 按 IP 限速，中继按原样转发、不放大 QUIC 初始包。
  - 代理协议是 `CHASTC1\n` + JSON 的 UDP 控制报文，与不透明数据靠魔数区分；注册只上传主机密钥的 SHA-256 摘要，`--server-key` 保护注册、统计和下线，重复注册 ID 但摘要不符会被拒绝。
  - 验证（2026-10-21，单机回环）：真实 chast 端到端在直连模式下 `frames=201 ... received=201 decoded=201 digest=E198527DF2B2B548`，同期中继字节为 0；强制中继模式下 `frames=202 ... received=202 decoded=202 digest=90C686753B88E5A2`，中继端口统计 `to_host_bytes:16402, to_viewer_bytes:2501405`。`go test ./...` 覆盖控制报文、注册表、HTTP API、会合/中继及代理三种模式（真实 UDP 回显 + 进程内服务器），并验证未知主机、错误密钥、第二查看者 `busy` 等失败路径。命令与数据见 `server/README.md` 的“验证记录”。
  - 设计取舍：不做账号体系，主机 ID + 密钥摘要就是最小注册凭据；中继不合并多个主机到一个端口，换取“服务器重启后按主机重新分配”的简单性与不混淆数据报的隔离性；直连失败是显式回退而不是伪装成功。
  - 已知限制：单机无真实 NAT，跨网锥形 NAT 打洞与对称 NAT→中继仍需两台异网机器手动复核（回退决策本身有单元测试）；Go 代理尚未嵌入 Flutter 查看者与托盘主机，当前需手动启动 `chast-tunnel`；中继是 UDP，无拥塞控制，弱网表现取决于 QUIC 自身；HTTP 默认明文，公网部署需在前置 TLS 反向代理后。
- 待做：M4.1 地址簿：设备列表与首页“最近连接”打通，客户端内置代理（或直接实现打洞），与首页身份/中继占位文案对应。

## M5 — 体验扩展

- 自适应码率/分辨率、RTT 与端到端延迟统计、弱网 QoS。
- 剪贴板同步、音频、文件传输。
- 更多采集/编码后端与其他平台。

## 推进原则

1. 每一阶段先补可重复的回环验证，再改 UI。
2. 协议字段只增不改；版本号随语义变化提升，ALPN 家族名不变。
3. 输入默认关闭，闸门在服务端；客户端不得绕过 `HelloAck` 的标志。
4. 键鼠事件逐条发送、按序应用；宁可丢新事件，也不阻塞视频路径。
