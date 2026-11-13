# 发射端改进实施记录（第一批）

日期：2026-11-12，集成与复验：2026-11-13。基于 [发射端 RustDesk 对比审核](sender-rustdesk-current-review-2026-11-12.md)；分支起始 HEAD 为 `deb64b89f558bd7b7d84abfb870a15a827da218f`（协议 v72），集成基线为 `main` 的 `bea63f9`（协议 v73、10 ms Opus）。

本批完成本地视频积压恢复、新观看者启动策略、主机通话 DSP 接入和可观测指标。接收端、服务器端生产代码没有改动。下面的测试验证实现行为；不代表已完成 RustDesk 二进制 A/B 或产品整体性能验收。

## 1. 视频队列：补充时间预算和恢复边界

实现：[desktop_stream.cc](../src/host/desktop_stream.cc)、[desktop_stream.h](../src/host/desktop_stream.h)。

- 在共享编码输出发布后记录时间，订阅者恢复读取或取编码包时，检查 250 ms 的等待预算。原有 60 帧 / 8 MiB 容量限制继续生效。
- 超时后清掉该订阅者的整条未发送依赖链，等待新 IDR；不从中间 P 帧续传，不重建共享编码器。
- 超时判断由恢复读取触发；永久停住的观看者不会单凭时间推移不断要求 IDR。保持无限 P 帧、按需 IDR，没有添加周期 GOP。
- 区分编码耗时、采集线程处理请求的耗时与订阅者自身等待；在请求发起时取样，保留此前排队时间，避免慢软件编码或低 FPS 反复触发错误恢复。
- 保留当前帧对象的生命周期，避免主机持有 `FrameInfo` 引用时因丢帧将其释放。

250 ms 不是总画面年龄或端到端呈现延迟上限。它只约束本地消费者积压，不能撤回已交给 QUIC 的旧数据，也不能消除采集、编码、线程等待和解码呈现耗时。

新增无需等待采集线程的 `DesktopStreamPool::queue_stats()`，并汇入 `HostServerStats`：

| 字段 | 含义 |
| --- | --- |
| `video_queue_wait_ms` | 最近采样订阅流最后一次交付给主机会话的本地等待时间；排除采集线程处理请求的等待 |
| `video_max_queue_wait_ms` | 本会话所有采样流的上述最大值 |
| `video_expired_frames` | 按时间过期清理的帧数，包含因依赖关系一起清理的帧 |
| `video_age_resyncs` | 按时间过期触发的恢复次数 |

这些指标可从 `HostServer::status().stats` 读取，不改变协议。`video_delivery_ms` 仍是既有接收确认链路指标，不能解释为接收端已完成解码/呈现。

## 2. 新观看者：不重新触发稳定共享流的低帧率启动

实现：[video_qos.cc](../src/host/video_qos.cc)、[video_qos.h](../src/host/video_qos.h)。

- 第一批观看者仍以 15 FPS 保守启动。
- 当已有观看者接入至少 1 秒，且已经初始化帧率、有至少两次连续良好反馈时，新观看者先继承当前共享帧率。
- 新观看者的首次反馈也以继承帧率为起点；重复接入不重新启动全局暖机。
- 新观看者明确请求的 FPS 上限、首包停滞和后续拥塞反馈继续参与共享决策。

本批隔离的是启动降帧，不是每观看者独立码率/分辨率/编码。慢端真实拥塞仍可能降低共享流；编解码偏好和画质投票也仍共享。

## 3. 通话：接通默认 AEC / NS 与播放参考

实现：[host_server.cc](../src/host/host_server.cc)、[host_settings.h](../include/chast/host_settings.h)、[host_settings.cc](../src/host/host_settings.cc)。

- 主机通话麦克风默认请求回声消除和降噪。底层继续使用已有 `AudioPreprocessor` / SpeexDSP。
- 给每个会话建立其自身来音播放参考；关闭系统声音发送时仍可用于主机语音 AEC。
- 只将播放器接受的 48 kHz 单声道/双声道 PCM 写入参考。播放失败或关闭时清空并停用参考；来音中断超过 500 ms 也清空，恢复时重新建立。
- 指定麦克风不可用后的默认设备回退、通话中换设备都保留 DSP 配置。换设备创建新的采集实例，DSP 随之重建；来音参考的生命周期继续跟随播放器。
- 停止主机向外发送语音不会单独关闭仍在运行的来音播放参考。

新增本地配置项，默认均为 `1`，支持保存、加载及提权交接使用的设置序列化；错误值保留默认并产生警告：

```ini
voice_aec=1
voice_noise_suppression=1
```

原有 `audio_aec` 继续控制系统声音回传的回声处理。新增开关目前通过配置文件设置，没有新增界面控件。实际 DSP 创建失败时，已有底层实现仍允许回退为原始麦克风信号，不能仅凭开关开启就宣称实机 AEC 已生效。

当前语音参考来自本会话来音，不是全主机所有应用/观看者的混音；固定参考延时与真实 WASAPI 播放时刻仍有差异。多人外放、耳机、蓝牙和双讲效果需要实机确认。

## 4. 构建与验证

本次使用独立目录 `build/integration-sender-freshness`，没有使用 `build/Release` 的旧程序作为验证证据。

```powershell
cmake -S . -B build/integration-sender-freshness -G "Visual Studio 18 2026" -A x64 -DCHAST_BUILD_TESTS=ON
cmake --build build/integration-sender-freshness --config Release --target chast-host chast-service chast-host-parity-test chast-desktop-stream-test chast-host-voice-test chast-host-settings-test chast-video-qos-test chast-audio-dsp-test chast-audio-codec-test chast-audio-playout-test chast-protocol-test --parallel 8
ctest --test-dir build/integration-sender-freshness -C Release -R '^(video_qos|desktop_stream|host_voice|host_settings|audio_dsp|audio|audio_playout|protocol|host_parity)$' --output-on-failure -j 3
```

最终结果：**9 / 9 项 CTest 全部通过，总计 177.22 秒**。`host_parity` 为 0 个失败场景；最终日志没有跳过用例。另单独运行 `chast-host-parity-test --freshness` 与 `--display-streams` 均通过。覆盖范围：

- 本地积压未达到帧数上限但已超时、`capture()` 与 `encode()` 之间停顿、IDR 恢复、其他观看者可继续读取、共享编码器不重建。
- 编码耗时大于时间预算、低帧率采集线程等待，不应误触发过期恢复。
- 真实 QUIC 回环中的主机会话停顿，恢复后统计能够反映过期帧与恢复次数。
- 首批/后加入/重复加入、明确 FPS 上限、首包停滞和离开后的 QoS 恢复。
- 合成桌面与麦克风、真实 Opus 编解码、模拟播放器；关闭系统声音下的来音参考、间断/重启、播放失败清理、换麦克风和显式 DSP 关闭。
- 现有多人、多屏、控制反馈与视频拥塞恢复集成场景。

`chast-host` 与 `chast-service` 均构建，但本次没有启动部署这些程序、安装服务/驱动、提交代码或操作真实键鼠/锁屏/UAC。

最终产物位于 `build/integration-sender-freshness/Release`；本机时间与 SHA-256 如下，便于固定后续验收实际运行的文件：

| 产物 | 本机最后写入时间 | SHA-256 |
| --- | --- | --- |
| `chast-host.exe` | 2026-11-13 09:36:24，Asia/Shanghai | `A243D4F95774C2CEA78DC655B34085EB613819C6F5D3A88879970F46568F5844` |
| `chast-service.exe` | 2026-11-13 09:36:34，Asia/Shanghai | `5443B3A3602EC0441F74A3C2B60FF4739C0E5A8F19F008BC886B25F34A2D1019` |

本次构建的相关测试通过不等于仓库全部 74 项 CTest 已运行；其余用例不纳入本次结论。

### 本机构建依赖说明

补装了 zstd 1.5.7、OpenH264 2.6.0、libvpx 1.16.0、OpenSSL 3.6.2、libyuv 1916 及其构建依赖，沿用工程既有的 vcpkg 前缀与 oneVPL 前缀。

libyuv 原下载源及 Git HTTPS 获取失败；通过 GitHub 镜像归档取得相同固定提交 `d98915a654d3564e4802a0004add46221c4e4348`，使用忽略目录 `build/review-deps-ports/libyuv` 的临时 overlay 构建。归档 SHA-512：

```text
3dcadbf1262a3c81d15b4eab86b004a5ddf811aa29bd37d712e97f7dab849844c00641967fefd90b2ab187569ced3f07b06441b86936e20e7b2dcf122007993d
```

本次构建仍报告已有依赖的 CRT 默认库冲突 `LNK4098`，没有通过屏蔽警告绕过。vcpkg 也提示 MSVC 的 libyuv 构建未开启其加速代码；这一产物适合功能回归，不能直接作为 VP9/4:4:4 转换成本的最终性能基线。正式性能验收前需要统一依赖 CRT 并确认 SIMD 构建参数。

日志保存在忽略的构建目录 `build/integration-sender-freshness/Testing/Temporary/LastTest.log`；早期 `host_voice` 的 10 ms 时序失败已修正并以最终通过的复验结果为准，中间失败不计为最终通过结果。

## 5. 下一批仍需完成的工作

1. 使用固定双方二进制、显卡/驱动和内容，在相同实际码率下做 RustDesk A/B；增加接收端解码完成和实际呈现时刻统计，才能讨论呈现 P95 / 最长停格。
2. 便携模式提权与安全桌面连续性：当前批没有改变现有交接架构；需要设计会话延续并进行 UAC、锁屏、解锁、用户切换实机矩阵。
3. VP9 和 4:4:4 质量/成本标定、音频过期包预算、截图 GPU 回读与多屏影响专项；本批没有直接套用 RustDesk 的编码参数。
4. 若共享流仍影响目标多人场景，再推进分组/分层或每观看者输出，当前改动不等同于这些架构。
5. 打印、角色互换、录制归档等跨端功能按目标客户需求另行推进。
