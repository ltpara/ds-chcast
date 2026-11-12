# VDENC vs FFmpeg QSV 同码率画质对比（1080p H.264）

状态：已完成（2026-11-11）。单机、真实桌面采集、真实编码器，
逐帧 PSNR/SSIM 对照。

## 目的

确认两条 H.264 硬编路径在 1080p、约 2.5–2.8 Mbps 下的画质是否一致，
并复核 `vpl_encoder.cc` 的 CBR 漏桶修复（本文件“修复”一节）。

## 修复背景

oneVPL 的 legacy `mfxhw64` 运行时在 `MFXVideoENCODE_Query` 时不回填
`BufferSizeInKB` / `InitialDelayInKB`。两个字段保持 0 时 BRC 没有漏桶
约束，实际码率可数倍于目标：

- 修复前：1920×1080、目标 2777 kbps / 30 fps、真实屏幕内容，
  实测约 5351 kbps；60 fps 元数据、约 58 fps 输入时约 10081 kbps。
- 修复：`src/codec/vpl_encoder.cc` 新增 `set_cbr_rate()`，
  同时写入 `TargetKbps`、1 秒 CPB（`BufferSizeInKB = Target/8`）、
  `InitialDelayInKB = Buffer/2`、`MaxKbps = Target`；初始化、
  原地 `set_rate`、完整重建、`reopen()` 四处统一调用。
- 修复后同样内容：目标 2777 / 30 fps 实测 2565 kbps；
  目标 2777 / 60 fps（实际约 57 fps 输入）实测 2493 kbps。

FFmpeg QSV 路径不受影响：`ffmpeg_encoder.cc` 用
`bit_rate = target - 1`、`rc_max_rate = target` 的 capped VBR，
libavcodec `qsvenc.c` 自己显式设置 CPB。

## 方法

- 内容：`build/quality/pattern.html` 的 1080p 屏幕共享场景
  （暗色编辑器、滚动代码、漂移会话窗、2 Hz 图表、闪烁光标），
  全屏 Chrome kiosk 置顶运行，刷新约 60 Hz。
- 采集：`chast-encode` DXGI 采集，`--dump-input` 逐帧导出编码器
  实际输入的 NV12。逐帧回读会把采集压到约 28 fps，因此两路都用
  `--framerate 30`，让元数据帧率与输入节奏一致，CBR 单帧预算可比。
- 编码：VPL（VDENC AVC）配置 2777 kbps；FFmpeg QSV 先按同配置
  2777 跑一组，再把配置目标提到 4000 跑一组，使实际码率与 VPL
  对齐（QSV capped VBR 在该内容上欠冲）。
- 度量：`chast-decode --dump-yuv` 解码，`build/quality_metrics.py`
  `--ref-dir` 模式逐帧配对 `input-%05llu.yuv` 计算 Y/U/V PSNR、
  8×8 块 SSIM。先做 −2..+2 帧偏移扫描，两路均在偏移 0 取得
  PSNR 峰值（约 55.7 / 56.4 dB，相邻偏移低 33 dB 以上），
  证明输入帧与编码帧一一对应。

命令（每路 6 秒）：

```powershell
build/Release/chast-encode.exe --display 0 --seconds 6 --codec h264 \
  --encoder vpl --bitrate 2777 --framerate 30 \
  --out build/quality/q/vpl.h264 --dump-input build/quality/q/vpl-input \
  --no-cursor --no-hdr --quiet
build/Release/chast-encode.exe --display 0 --seconds 6 --codec h264 \
  --encoder ffmpeg --bitrate 4000 --framerate 30 \
  --out build/quality/q/ffmpeg-4m.h264 \
  --dump-input build/quality/q/ffmpeg-4m-input --no-cursor --no-hdr --quiet
build/Release/chast-decode.exe --in build/quality/q/vpl.h264 --codec h264 \
  --dump-yuv build/quality/q/vpl-dec.yuv --quiet
PYTHONPATH=build/pydeps python build/quality_metrics.py \
  --ref-dir build/quality/q/vpl-input --test build/quality/q/vpl-dec.yuv \
  --label vpl --csv build/quality/q/vpl-metrics.csv
```

## 结果

同一码率段（实际约 2.5 Mbps）：

| 运行 | 配置目标 | 实际码率 | 帧数 | PSNR-Y 均值 | 中位 | 最低 | SSIM-Y 均值 | SSIM 最低 | 池化 PSNR-Y |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| VPL A | 2777 | 2565 | 169 | 55.76 | 57.01 | 43.57 | 0.9996 | 0.9980 | 54.10 |
| VPL B | 2777 | 2466 | 166 | 55.80 | 56.39 | 46.73 | 0.9997 | 0.9988 | 55.21 |
| FFmpeg A | 4000 | 2549 | 165 | 56.41 | 61.07 | 44.80 | 0.9995 | 0.9975 | 52.01 |
| FFmpeg B | 4000 | 2513 | 163 | 55.03 | 59.68 | 38.34 | 0.9989 | 0.9887 | 48.49 |

同配置目标（2777）：

| 运行 | 实际码率 | 帧数 | PSNR-Y 均值 | SSIM-Y 均值 |
| --- | --- | --- | --- | --- |
| VPL | 2565 | 169 | 55.76 | 0.9996 |
| FFmpeg | 1747 | 164 | 52.61 | 0.9990 |

结论：

1. 实际同码率下两条路径画质基本一致：Y-PSNR 均值 55.8 vs 55.7
   （两轮平均），SSIM 0.9996 vs 0.9992，屏幕内容接近透明。
   差异落在内容相位噪声内——VPL 重复运行均值差 0.04 dB，
   FFmpeg 两轮差 1.4 dB（少数运动帧更差，B 轮出现 1 帧逐字节无损）。
2. 码率控制风格不同：VPL CBR 用满预算，逐帧质量平稳（标准差小）；
   FFmpeg QSV capped VBR 在静止帧给到更高画质（中位数 56–61 dB，
   甚至无损帧），运动帧下探更深，且在该内容上欠冲目标 37%
   （2777 配置只发 1747）。
3. 产品配置不变：H.264 首选 VDENC，FFmpeg QSV 作为回退。CBR 漏桶
   修复只影响 VPL，不改变弱网策略（VideoQos 的码率/帧率阶梯）。

## 限制

- 只有 PSNR/SSIM，没有 VMAF（本机无模型）。
- 单机顺序采集：两次运行是不同 6 秒窗口，动画相位不同，不能逐帧
  跨编码器配对；每轮只与自己的输入对齐（偏移已校验）。
- `--dump-input` 的逐帧回读把采集压到约 28 fps，因此用 30 fps
  元数据；生产路径由 DesktopStreamPool 按目标帧率配速，无此开销，
  本结果属于编码器级对比，非端到端。
- 未测低码率（<1 Mbps）和高动态内容；弱网低码率行为仍以
  `docs/weak-network-acceptance.md` 为准。

## 2026-11-12 软件回退 H.264 vs VP9 同帧对比（追加）

状态：已完成（编码器级同帧 A/B，未跑 RustDesk 二进制，未改产品默认）。
问题：无硬件编码时 Auto 回退 OpenH264，RustDesk 回退 libvpx VP9；
需要同内容数据判断该差异是否值得改。

### 方法

- 参考帧：一次 2560×1440 硬件 VPL 采集，`chast-encode --dump-input`
  逐帧导出编码器实际收到的 NV12，内容为全屏 Chrome 页面
  （暗色编辑器、滚动代码、图表演示、浮窗与日志），216 帧 @30 fps
  元数据。四组测量回放完全相同的帧，消除跨运行的动画相位差。
- 1080p 组：同一批 NV12 用 libyuv `kFilterBox` 缩到 1920×1080，
  度量以缩放后的帧为参考，衡量的是编码器在同一输入上的差异。
- 编码/解码：离树探针（`build/quality/probe.cc`）把 NV12 转 BGRA
  （BT.601 limited），用 `create_encoder(..., backend=Software)` 走
  生产 OpenH264 / libvpx 配置，逐 access unit 交给生产解码器
  （`copy_frames=true`），再对解码 luma 计算 Y-PSNR 与 8×8 块 SSIM。
- 码率：生产公式、Balanced 0.67。1440p：H.264 4000 kbps、
  VP9 3005 kbps；1080p：H.264 2778 kbps、VP9 2083 kbps。
- `encoder_fps` 只计时 `encode_cpu_bgra`，不含 BGRA 转换、解码和度量。
- 区域指标：左侧 58% 宽为编辑区，其余为侧栏；表中只列编辑区，
  整帧/侧栏逐帧数据在 `build/quality/*-real.log` 与 `.csv`（未入库）。

### 结果（216 帧，单机空闲）

| 分辨率 | 编码器 | 目标/实际 kbps | encoder_fps | PSNR-Y 均值/最低 | SSIM-Y 均值/最低 | 编辑区 PSNR/SSIM |
| --- | --- | --- | --- | --- | --- | --- |
| 2560×1440 | OpenH264 2.6.0 | 4000 / 4173 | 15.6 | 44.02 / 40.88 | 0.9947 / 0.9881 | 43.08 / 0.9957 |
| 2560×1440 | libvpx 1.16 VP9 | 3005 / 2994 | 52.0 | 40.68 / 35.69 | 0.9872 / 0.9702 | 39.94 / 0.9907 |
| 1920×1080 | OpenH264 2.6.0 | 2778 / 2780 | 25.9 | 43.49 / 39.64 | 0.9923 / 0.9826 | 42.49 / 0.9933 |
| 1920×1080 | libvpx 1.16 VP9 | 2083 / 2070 | 83.7 | 40.61 / 36.03 | 0.9832 / 0.9644 | 39.75 / 0.9857 |

四组均为 216/216 帧出包、1 个关键包、无丢帧；H.264 实际码率略高于
目标，VP9 基本贴目标。

### 结论

1. **画质**：同内容同画质档下，H.264 的 PSNR 高约 2.9–3.3 dB、
   SSIM 高约 0.007–0.009，代价是按生产公式多用约 34–39% 码率。
   VP9 的绝对画质仍在 40.6 dB / 0.983–0.987，差距属于“更好”而非
   “可用性”级别。
2. **吞吐**：OpenH264 在本机 1440p 只有 15.6 fps、1080p 25.9 fps
   （生产配置最多 4 线程），均达不到 30 fps；libvpx 用 row_mt / 8 线程
   达到 52.0 / 83.7 fps。无硬件的 1440p 主机上，H.264 回退会因编码
   吞吐不足而降帧；VP9 能维持 30 fps。
3. **码率饱和**：1080p 把 H.264 目标从 2.78 Mbps 提到 6 Mbps，实际
   只从 2.78 升到 2.82 Mbps，PSNR 43.5 → 43.7 dB——该内容下编码器
   已到质量上限（QP 下限），不是码控失效。
4. **取舍**：H.264 偏“同码率更高画质、兼容性好”，VP9 偏“低码率、
   高吞吐、画质略低”。RustDesk 的无硬件回退是 VP9；本次数据支持
   把 Auto 的无硬件回退改为 VP9 以对齐吞吐和码率，但这是产品决策，
   还要补查看端软解开销与低端机 CPU 验证。本轮不改产品默认。

### 度量修正记录

中间版本探针用解码器返回的 `pts_ms` 配对参考帧。FFmpeg 的 H.264
解码器不透传调用方 pts（每帧都报 0），导致所有 H.264 帧都与参考第 0
帧比较，得到与码率无关的约 20–22 dB 假结果。最终版本改为按编码包
`pts_ms` 配对并重跑；VP9 解码器透传 pts，不受影响。此前若引用过
20 dB 级 H.264 软件画质，应以此节数据为准。

### 限制

- 单机（编码线程：OpenH264 ≤4、VP9 ≤8）、单一内容、仅 luma 指标，
  无 VMAF、无主观 ABX。
- 离树回放只覆盖编码器，不含采集/传输/查看端解码；1080p 由 1440p
  缩放得到，不是原生 1080p 采集。
- 未运行 RustDesk 二进制，跨产品观感仍以双机验收为准。

## 产品决策：软件回退改为 VP9（2026-11-12 晚）

基于上面的同帧数据（VP9 吞吐约为 OpenH264 的 3.3 倍，H.264 的画质优势以
34–39% 额外码率为代价），产品决定向 RustDesk 对齐：

- 产品软件回退改为 **VP9（libvpx）**，OpenH264 编码器、CMake 依赖与
  `OpenH264::openh264` 链接全部移除，不再提供软件 H.264。
- `EncoderBackend::Auto` 的路径为 oneVPL → FFmpeg 厂商编码 → 软件 VP9；
  `Software` 只接受 VP9，其余编码格式的请求明确报错，不静默换格式。
- 硬件连续失败后的恢复改为协商软件 VP9；CPU 采集（`GdiCpu`）只允许 VP9。
- 固定 `Vpl`/`Ffmpeg` 的 VP9 请求继续报“该后端没有 VP9 编码器”，不回退。

证据等级：编码器级同帧实测 + 自动化测试（本轮构建与 CTest 记录见
[sender-effectiveness-rustdesk-2026-11-11.md](sender-effectiveness-rustdesk-2026-11-11.md)
第 17 节）。未做真实双机链路、低端机软解 CPU 与主观 ABX 验收；这些仍是
后续人工验证项。
