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
