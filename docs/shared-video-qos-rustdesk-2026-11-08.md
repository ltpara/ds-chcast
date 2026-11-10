# 共享视频质量控制与 RustDesk 对照

目标仍是完成功能复刻。本轮处理多人共享视频的质量决策，并不表示整个复刻目标已经完成。

参考 `D:/rustdesk-ref` 的 `7423dced37845c48093b6f71bd82e4a16cc8c79d`：`src/server/video_qos.rs`、`src/server/connection.rs` 的 TestDelay 路径，以及 `libs/scrap/src/common/codec.rs` 的质量系数。

## 已接入的行为

| 规则 | 实现 |
| --- | --- |
| 所有观看者共同参与 | `DesktopStreamPool` 持有一个 `VideoQos`，每条桌面连接一个 `QualityPeer`；跨屏仍参与同一个控制器 |
| 请求 FPS | 取全部观看者上限的最小值，范围 1–120，主机配置仍是上限 |
| 新连接 | 自适应模式从 15 FPS 起步，每个观看者自己的第一秒不超过 15 FPS |
| 延迟反馈 | 主机发起 LatencyProbe，客户端自动回复；最多一项未完成探测，过期回复不能替换当前样本 |
| 未回复 | 等待超过 2 秒后每秒减半，最低 `MIN_AUTO_FPS`=5 FPS；回复或离开后解除约束 |
| RTT 校正 | 20 样本滑动窗口；only 明显抬高的近期最小值才抬升基线，单次增幅不超过 50 ms |
| FPS 调整 | 按参考代码的 50/100/150/200/300/600 ms 阈值、质量对应最小 FPS 与连续好样本计数调整；启动期每两个干净回复翻倍，拥塞恢复回到拥塞前档位并带 5 样本恢复守卫 |
| 画质 | 最近一次明确的画质设置生效；单独改 FPS 不抢占其他人的画质设置；再次选中原来的预设可重新成为最近设置 |
| 质量比例 | High/Best 1.5，Balanced 0.67，Smooth/Low 0.5，传入既有硬件码率计算；手工配置的码率仍作为上限，保留音频预算 |
| 比例调整 | 每 3 秒看最差用户延迟；3 秒窗口内任一显示服务至少编码 6 次才允许上调；按 0.95/0.9/0.7/0.85/0.5/0.8 阶梯下调，上调按 15/10/5% 且每次最多增加 150 kbps |
| 生命周期 | 切屏保留连接的策略参与者，离开删除；最后连接离开重置策略。显示源销毁时清理对应计数 |
| 编码器调用 | 只有显示服务线程应用聚合结果；连接自己的 `set_rate` 适配器只同步共享结果，不再覆盖其他用户的限制 |

移除了桌面会话中独立的 `BitrateController` 调节路径，保留该类和原测试作为其他用途的纯逻辑工具。服务缓存已成功提交的目标，防止硬件码率取整后每个采集周期重复调用 `set_rate`。码率或 FPS 的实际变化通过 StreamInfo 通知所有观看者。回归过程中还修复了关键帧请求的异步重复：已有当前代次 IDR 等待该订阅消费时，不再重复安排编码，保持静止桌面不自行产生画面。

v51 引入双向 LatencyProbe/Reply；客户端的时钟同步保留，主机测量不依赖客户端墙钟。当前协议进一步升为 **v52**，加入下面的视频取帧确认；主机与查看器须同步更新。因为 QUIC 回调不能阻塞，回复发送队列满时可丢弃；主机重发同一时间戳，保留首次探测的等待时间。发起探测也改成不等待发送容量；队列满时保留最初时间戳，不能把拥塞等待清零。

## 视频积压与恢复（v52）

参考的 RustDesk 连接在 `rx_video.recv()` 时通知视频服务；要求视频 ACK 的连接则在 `VideoReceived` 到达后通知。每屏 `VideoFrameController` 等待已发送连接的取帧通知，最多 3 秒，控制消息通过 TestDelay 参与共享质量策略。本项目 QUIC 控制流和视频流独立，不能直接把控制探测耗时当成视频耗时。

因此增加 `VideoReceipt(u64 sequence)`：查看器的 `receive_frame()` 返回完整访问单元时发送累积确认。主机仅接受当前待收序号范围的确认，忽略重复、过期和超前值；延迟使用主机保留的采集时刻计算，包含源队列、发送、网络及观看端取帧等待。新视频延迟样本、最老待收帧年龄和控制探测共同进入全局策略；静态画面不反复使用旧的视频延迟。确认遇到发送队列满会保留最新序号，在下次取帧或收到主机探测时重试。

每条连接的视频待收窗口最多 60 个访问单元、约 8 MiB；最老单元发送后超过 500 ms 也停止追加，单个超大 IDR 可以进入空窗口。连接线程使用非阻塞视频发送，窗口满仍处理切屏、探测、音频和其他控制操作。恢复后清理该订阅的待发压缩帧，并等待新 IDR；其他订阅和共享编码器继续工作。发送被拒绝不消耗传输序号。

弱网回归另发现显示服务与连接线程重复按 FPS 等待。降帧后的第二次等待使旧画面继续积压，延迟反馈阻止恢复。现仅由显示服务决定采集编码节奏，连接及时取帧、发送和处理控制。

参数切换回归还暴露异步格式更新的时序：显示服务已换尺寸/编码格式，而连接仍持有旧描述时，不能先把新编码器码率广播到旧描述上。`read_rate()` 在同一条服务命令内核对格式并取码率/FPS；不匹配就等新画面携带的完整格式更新，避免重复公告。

这些机制实现了视频背压参与质量决策和各连接独立响应，**并非逐项复制 RustDesk 的等待算法**：RustDesk 每屏同步等待通知，本项目采用各连接有限窗口和独立恢复。不同屏仍按 RustDesk 的全局 QoS 共同决定 FPS/画质，不保证慢连接存在时其他屏仍保持其原始 FPS。

## 2026-11-09：按当前 RustDesk `video_qos.rs` 重写策略

参考 `D:/rustdesk` 的 `c7f212d`（`src/server/video_qos.rs`），只移植行为与字段语义，不引入 RustDesk 代码或依赖：

- 每用户延迟状态：2 样本延迟历史、20 样本 RTT 窗口；只有窗口近期最小值不明显高于旧最小值（+10 ms 容差）时才以一半幅度抬升基线，单次增幅 1–50 ms。只有新回复才更新 RTT；探测超时按秒降帧，不重复累计 RTT 样本。
- 拥塞确认：FPS 需 3 个连续坏样本；码率需 2 个连续坏样本或 2 次探测超时。`replies_after_bitrate_reduction` 把降码率前已在途的探测排除在新证据之外。
- FPS 恢复：`limit_fps_change` 在恢复后 5 个样本内再次拥塞、或 600 ms 以上延迟时回退目标；除数 5，延迟 ≥1000 ms 或恢复失败时用 2。2–3 个干净样本恢复到 `fps_before_congestion`；若恢复档位立刻拥塞，则把记忆档位再降 1/4。
- 启动加速：两个干净回复后 FPS 翻倍，遇到 50 ms 以上超额、自动刹车或到达观看者上限即结束。
- 探测超时：等待超过 2 秒后每秒减半，最低 `MIN_AUTO_FPS`=5 FPS，且只降不升。chast 每 500 ms 采样，按整秒桶去重，避免把同一秒计算两次。
- 码率阶梯与增长上限：按 0.95/0.9/0.7/0.85/0.5/0.8 阶梯下调，确认后才降；上调只在动态画面窗口进行（3 秒窗口至少编码 6 帧），按 15/10/5% 步进，单次最多 +150 kbps，并受当前画质目标与比例下限约束。
- 画质投票：显式画质投票才推进票序，FPS-only 投票保留其他观看者最近的画质；再次选中同一预设视为一次新的显式投票（主机 `quality_change` 的 `!cap_change && !preference_change` 分支）。

当时 `adaptive_bitrate=false` 同时冻结 FPS 和码率，只聚合观看者的显式上限；该语义已在 2026-11-10 拆分，见下文。`application_latency` 与 `video_backpressure` 走自适应路径，验证 15 FPS 起步、干净回复升帧，以及未取帧时降至 5 FPS 并在恢复消费后回升。

本轮验证：

- `chast-video-qos-test` 99 项检查通过，覆盖启动加速、恢复守卫、探测超时、比例阶梯与投票语义；日志 `build/video-qos-final.log`。
- `chast-desktop-stream-test` 353 项检查通过，共享策略与编码器生命周期保持不变；日志 `build/desktop-stream-final.log`。
- `chast-host-parity-test` 全量场景 0 失败，含跨屏多人画质投票、未消费视频降至 5 FPS、恢复后回到更高 FPS 与连续序号；日志 `build/host-parity-final.log`。

## 2026-11-09：静止刷新对齐 `static_refresh.rs`

参考 `D:/rustdesk` 的 `c7f212d`（`src/server/video_service/static_refresh.rs`），只移植行为与字段语义，不引入 RustDesk 代码或依赖：

- **预算**按编码器、厂商后端与画质比例分档：VP9 为 40/20/20/20；H.264 与 HEVC 区分 NVENC、QSV、AMF 和其他后端；AV1 固定为 0，不做静止刷新。画质比例仍来自共享 QoS 的 0.5/0.67/1.5 分档。
- **节奏**：间隔取 `max(1/fps, 1/30)`，即不超过 30 fps；第一帧和 VP9 的间隔至少 100 ms。计数随服务 tick 量化，实际约 100–116 ms 一次。
- **停止与重置**：VP9 允许刷新失败（libvpx 可合法丢帧），其他编码器连续 3 次失败停止；累计 100 次无输出停止；只有真实桌面变化（`nv12_updated`/`desktop_updated`）才把预算计数清零并重新等待首个 IDR，纯光标采样不会续期。
- **不计入动态画面**：刷新发布前清除图片的 `nv12_updated`/`desktop_updated`，刷新帧不冒充新的桌面更新，也不计入共享 QoS 的发送计数和动态画面窗口；只有真实捕获帧提供 `dynamic` 证据。2026-11-11 复核 `D:/rustdesk` 的 `src/server/video_service/static_refresh.rs`：`try_encode` 直接调用 `encoder.encode_to_message`，不增加 `send_counter`；固定基线 `7423dce` 没有该模块，只有旧版软件 YUV 重复路径会累加。2026-11-10 曾把刷新帧计入并声称对齐 `send_counter`，该判断错误，已撤销。静止桌面在拥塞降码率后要等真实画面更新才恢复码率，与参考一致；本机弱网测试的桌面有持续真实更新，回升不依赖刷新帧。刷新失败仍回退到一次新 IDR，不改变后端。
- **传输**：刷新帧不领取视频待收窗口额度、不为停滞的观看者等待，也不进入延迟统计；仍受 QUIC 在途发送上限约束，冻结的显示端不会因此无限增长队列。
- **顺序修复**：发布 IDR 时清空该订阅已排队的旧刷新帧。此前静止刷新以最高 30 fps 续填队列，适配器在等待 IDR 时丢弃所有非 IDR，多屏拓扑重建窗口内会出现请求 IDR 被掩埋、观看端只收到另一块屏刷新帧的饥饿。

验证（本机 Release）：

- `chast-desktop-stream-test` 408–410 项检查通过，连续 3 次 0 失败；覆盖预算表、静止刷新计数、预算耗尽停止、新画面重启及刷新不计为编码帧。日志 `build/desktop-stream-rerun1..3.log`。
- `chast-host-parity-test` 全量 976–977 项检查，连续 3 次 0 失败；`--topology` 连续 4 次通过，覆盖多屏重排、几何重建和静止桌面关键帧。日志 `build/parity-final-run1..3.log`、`build/parity-topology-fix-run1..4.log`。
- 真实 Intel GPU 双查看器 `chast-host-parity-test --hardware` 92 项检查 0 失败，含静止桌面请求硬件 IDR、共享 GPU 访问单元解码和剩余观看者继续取帧；日志 `build/parity-hardware-final.log`。
- Release ctest 64/64 通过、0 失败，总耗时 1058.29 秒；日志 `build/ctest-final.log`。
- 2026-11-10 弱网恢复判据修正：`chast-quic verify --netem`（`quic_weak_network`）连续 4 次通过，1000 kbps 瓶颈下最低目标 1000 kbps、恢复阶段回升到 1749 kbps 与 30 fps，访问单元全部送达并解码；日志 `build/weak-abr-v67-run1..4.log`。恢复判据取“最低目标之后出现过更高目标通告”，会话末尾的合法回退不再抹掉已经发生的恢复（当时把回升归因于刷新计入，该归因已被 2026-11-11 的复核否定）。
- 2026-11-11 撤销刷新计入后的复测：`video_qos`、`desktop_stream`、`quic_weak_network` 定向 3/3 通过（`build/ctest-v67-repeat-revert-1.log`）；`chast-quic verify --netem` 直跑 2 次，496/492 个访问单元全部送达并解码，最低目标 1000 kbps、回升 1749 kbps、30 fps（`build/weak-abr-v67-revert-run1..2.log`）。本轮回升由持续的真实画面帧提供 `dynamic` 证据，刷新帧仍不计入；完全静止桌面不会仅凭刷新帧回升码率，与参考一致。撤销后全量 Release ctest 70/70 通过（917.55 秒，`build/ctest-v67-full-4.log`）。全队列饱和的桌面流基线仍等待队列高水位稳定后取样。
- 2026-11-11 调价入口节奏对齐：`VideoQos::tick` 之前在每个服务 tick 都会进入 `adjust_ratio`；高延迟未确认的早退不更新 `last_ratio_`，动态计数因此约每 33 ms 被清零。现在周期入口增加 1 s 节流（完整调价仍保持 3 s 冷却），与 RustDesk 每秒一次的 `update_display_data` 一致。`video_qos` 新增确定性断言：先降比例制造余量，在 160 ms 超额延迟下逐 tick 送帧，1 s 后延迟恢复时必须带着累积的 `dynamic` 证据回升；去掉节流该断言即失败。`video_qos` 110 项通过（`build/video-qos-cadence.log`），`desktop_stream`/`quic_weak_network` 同轮定向 3/3 通过（`build/ctest-v67-cadence-1.log`）。全量构建还发现并修复 `tests/protocol_test.cc` 中 `preset_percent` 的重复定义（既有工作区编译错误），`protocol` 测试通过。节奏修复后的全量 Release ctest **70/70** 通过（946.63 秒，`build/ctest-v67-full-5.log`；2 项剪贴板监听测试按既有条件跳过），完整 `go test ./...` 同轮通过。
- 边界：刷新画质/功耗收益只在真实 GPU 回环上验证了“可解码、继续取帧”，未做跨机画质对比；预算表来自 RustDesk 实测值，本机显卡不保证同样适用。

## 验证入口

- `tests/video_qos_test.cc`：确定性时间与延迟序列，包括 FPS 聚合、最近画质、退出清理、超时、RTT 校正、静态/动态码率、150 kbps 增长限制及自定义质量的比例下限。
- `tests/desktop_stream_test.cc`：两块模拟显示器共享全局 FPS/质量，连接退出恢复，旧连接直接设置码率不能覆盖共享策略，编码器不重建。
- `tests/host_parity_test.cc`：16 个场景；包含跨显示器请求、重新选择预设、退出后恢复、切屏保持设置、双向探测，以及控制探测正常而视频停读时限至 5 FPS、恢复 IDR/连续序号和 FPS、慢连接停读时另一块屏继续发送、停读期间切屏。
- `chast-host-parity-test --hardware`：实际 GPU 双查看器，启用自适应质量，直接解码到各自 D3D11 设备。
- `quic_stream_options`：手工格式/参数变化的硬件回归。此测试明确关闭网络自动调节，以测量精确的手动 FPS 和关键帧成本；网络决策由上述独立测试覆盖。起点改成明确的 High/Best 1.5，保留原有纯码率变化不应产生额外 IDR 的断言。

最终 v51 验证通过：五组 CTest（含 14 个主机场景）全部通过，真实 GPU 双查看器通过，硬件参数切换通过；纯画质修改没有额外 IDR 的原断言通过。`chast-host`、`chast-service`、`chast-quic`、`chast-viewer-probe` 构建成功。日志为 `build/shared-qos-v51-final-build.log`、`build/shared-qos-v51-final-tests.log`、`build/shared-qos-v51-final-hardware.log`、`build/shared-qos-v51-final-options.log`。没有安装或重启系统服务。

v52 验证：

- 五组 CTest（含 16 个主机场景）通过，37.20 秒；日志 `build/video-receipt-v52-final-tests.log`。新增格式快照、恢复丢弃旧引用的单元断言也通过。
- 真实 Intel GPU 同屏双查看器通过，持续输出 D3D11 纹理，首位离开后不重建采集/编码；日志 `build/video-receipt-v52-hardware.log`。
- `quic_stream_options` 在格式快照修复后通过，36.15 秒；纯码率修改不增加 IDR、公告数量和收发解码完整性等原断言保留，日志 `build/video-receipt-v52-format-options.log`。
- 本机真实硬件编码 + QUIC 网络损伤代理通过：80 ms 单向延迟、20 ms 抖动、3% 丢包、1 Mbps，4–12 秒施加损伤，随后恢复。259 个发送访问单元全部接收并解码，零序号缺口/CRC/解码错误；最低目标 1536 kbps，最终恢复 30 FPS，最大帧年龄 2695 ms，最后视频接收延迟 9 ms、待收年龄 0。该次通过日志位于 `build/video-receipt-v52-final-net.log` 的弱网测试部分；同文件之前的参数切换失败已由上一条的独立通过日志替代。
- 弱网测试移除了旧独立 ABR 专有的“必须自动降分辨率”和固定 500 kbps 下限要求，改为保持所选显示几何、恢复 FPS、积压清空等断言；仍要求码率下降及恢复、视频完整到达和解码。比例下限继续由 `video_qos` 确定性测试验证。
- 最终 `chast-host`、`chast-service`、`chast-viewer-probe` 构建通过，`chast-quic` 随格式回归构建通过。日志 `build/video-receipt-v52-products-final.log`、`build/video-receipt-v52-format-build.log`。保留已有链接警告，未安装或重启服务；代码空白检查通过。

网络损伤代理验证不等于两台异地设备实测；真实 GPU 双查看器是本机同屏，异屏仍由不同尺寸的确定性媒体验证。

## 2026-11-10：全部订阅队列满时暂停生产

RustDesk 的 `video_service` 在每帧发送后最多等待 3 秒，直到所有订阅者从各自的 `rx_video` 取走该帧；慢观看者因此直接约束采集/编码节奏。本项目的订阅队列只在 `Source::call` 的服务线程上读写，若在该线程内同步等待连接取帧会与取帧命令互锁，不能照搬这一等待。

改为等价的高水位反压：每个订阅仍有 60 帧 / 8 MiB 的独立队列上限；`Source::pump()` 在推进下一个 tick 后检查全部存活订阅，**所有**队列都满时直接返回，不再采集和编码。只要还有一个观看者有空间，服务继续为该观看者生产，满队列的观看者保持原有的“丢弃旧引用、等待新 IDR”恢复路径。这样既消除了“所有观看端都停住后仍持续采集/编码并反复强制 IDR”的空转，也保留了本项目“一个慢连接不拖住其他观看者”的隔离性。tick 先推进再返回，避免反压期间忙等；暂停前已置位的 `keyframe` 在恢复后的第一帧继续生效。

验证：

- 新增 `slow_viewer_backpressure`：单观看者不取帧时，队列填充到窗口上限后 `captures`/`encodes`/`idrs` 在 500 ms 内完全不变；交付一帧后采集与编码立即恢复。既有慢订阅者溢出、静态刷新和双观看者隔离断言全部保留，`desktop_stream` ctest 通过（11.67 s）。
- 真实 1080p H.264 回环复核：`chast-quic serve/connect`，1920×1080、6000 kbps、30 fps，接收端 346 帧 / 11.985 s（约 28.9 fps），`crc-errors=0 decode-errors=0 gaps=0`，帧龄平均 8.1 ms、最大 43.2 ms，与修改前 28.7 fps 基线一致。该次只有一个持续取帧的观看者，未在真实 QUIC 上触发反压；暂停/恢复的行为证据来自确定性 `DesktopStreamPool` 测试。

## 2026-11-10：拆分 enable-abr 与 FPS 自适应

RustDesk 的 `abr_config`（`enable-abr != "N"`）只通过 `in_vbr_state()` 门控质量比例：关闭后 `adjust_ratio` 不运行、比例回到 `latest_quality().ratio()`、`can_reduce_bitrate()` 为假；延迟驱动的 FPS 调整（`user_network_delay`、`user_delay_response_elapsed`、`adjust_fps`）始终运行。本项目此前用同一个 `adaptive_bitrate=false` 同时冻结 FPS 和码率，配置语义与参考不同。

现拆成两个开关：

- `adaptive_bitrate` 只表示 ABR 质量比例，语义与 RustDesk 一致；关闭时 `variable()` 为假，比例保持观看者画质目标，FPS 仍按延迟、探测超时和启动加速调整。
- `adaptive_fps`（默认 true）只在需要精确手动速率的探针中关闭，固定为观看者上限的聚合值。`quic_stream_options` 与不带 `--netem` 的 verify 探针同时关闭两者；`host_media_stub` 为确定性测试关闭 FPS 自适应。该开关不进入 `host.ini`，生产配置始终开启。
- `host_parity_test` 的 `application_latency`、`video_backpressure`、`hardware_shared_display` 显式同时开启两个开关，保留原有的自适应场景。

验证：

- `chast-video-qos-test` 104 项检查通过。新增断言：ABR 关闭时仍从 15 FPS 起步、干净回复升至 60、确认拥塞后降帧，且比例始终为 1.5；`adaptive_fps=false` 时 400 ms 延迟和 3 秒探测超时都不改变配置速率。
- `desktop_stream` ctest 通过（11.22 s）；`host_parity` 全量通过（172.81 s），其中 `application_latency`、`video_backpressure` 和硬件场景现在显式同时开启两个开关。
- 真实 1080p 回环：`quic_stream_options` 通过（39.99 s），手动格式/参数切换仍按精确 FPS 和码率断言；`quic_loopback` 通过（39.71 s）。两者都在 `adaptive_fps=false` 下运行。

## 仍需继续完成或验收

1. v52 已补视频取帧反馈、有限发送窗口和恢复，控制流绕过视频积压的缺口已处理；2026-11-10 又补了全部队列满时暂停生产。RustDesk 的逐帧同步等待仍与上述高水位方案不同；其他流（例如大文件）的发送容量和极端拥塞需单独验收，不能据此声称 F06 全部等效。
2. 编码器拒绝设置新码率时保留已有流，按间隔重试，并将该显示源标为不能动态改变质量；跨不同硬件的失败和恢复还需专项验证。
4. v53 已接入所有桌面连接的全局编码交集和偏好多数票，见 [全局编码协商](D:/ds-chcast/docs/global-codec-rustdesk-2026-11-08.md)。色度协商、摄像头并入该策略、单连接多个独立视频流仍在后续范围。实际双屏、跨机弱网以及 Windows 桌面/驱动恢复未由本机回环代替。

完整剩余模块继续以 [发射端模块审核](D:/ds-chcast/docs/host-rustdesk-module-comparison-2026-11-08.md) 和 [完整功能审核](D:/ds-chcast/docs/rustdesk-full-parity-2026-11-07.md) 为清单；其中历史行号与修复前描述需按当前代码核实。后续还包括按连接 ID 的本地管理、服务审批和会话内提权、防休眠、主机录制、剪贴板方向和惰性文件内容、文件分页/空目录、键盘模式、打印及角色互换等，未在本轮重新定义为完成。
