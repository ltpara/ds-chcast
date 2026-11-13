# 指针四项复核与验收（2026-11-11，P2；2026-11-14 续验）

本文件记录 P2 指针四项（仅查看远端指针、隐藏指针语义、512 px 光标、所有者标记）对照 RustDesk 固定提交 `7423dce` 的复核结果、本机 GUI 证据、已修缺陷和剩余边界。2026-11-14 又补入 Windows 辅助功能指针倍率、DPI 派生和真实 GDI 三档采集结果。`build/` 下的脚本、截图与日志只在本机保留，不进入仓库。

## 1. 结论

| 项目 | 状态 | 证据与说明 |
| --- | --- | --- |
| 仅查看远端指针 | 已修正，GUI 复核通过 | DXGI 只在 `LastMouseUpdateTime != 0` 的帧里填写 `PointerPosition`；旧代码在每帧读取，把“本帧没有鼠标更新”误判成指针隐藏，查看端叠层随之闪烁/消失。修正见 [src/capture/dxgi_capturer.cc:891](../../src/capture/dxgi_capturer.cc)。查看端在非控制状态按 33 ms 轮询光标，用最新 sprite 绘制叠层；本机 4611 会话截图 `build/gui-ptr-vis-base-crop.png` 中可见叠层箭头。 |
| 控制模式自定义光标 | 已核实 | [viewer_bridge.cc:3420](../../app/windows/viewer/viewer_bridge.cc) 的 `ApplyCursor` 从 33 ms 轮询中取最新形状，`RemoteCursor::Activate` 安装当前缩放的自定义光标；主机报告隐藏时保留最后形状，不把控制端指针恢复成系统箭头（RustDesk 同语义）。 |
| 所有者标记（显示我的光标） | 已修正，GUI 复核通过 | 查看端 `showCursor`（x/y/visible，33 ms 节流）→ `ShowCursor` 控制消息 → 主机 `take_show_cursor_hint`（[src/host/host_server.cc:5666](../../src/host/host_server.cc)）→ `CursorHintOverlay`。旧实现用 `SetLayeredWindowAttributes(..., LWA_ALPHA)` 整体透明、`WM_PAINT` 只画图标，留下 32×32 黑色方块；现改为 32bpp top-down DIB + 自绘抗锯齿箭头（白填充、黑描边、4×4 子采样）并以 `UpdateLayeredWindow(ULW_ALPHA)` 提交逐像素 BGRA，窗口仍置顶、点击穿透、不激活。本机真实回环（`build/gui-pointer.ini`、查看端 `view_show_my_cursor=true`）查看端 stderr `showCursor x=1002 y=548 vis=1 queued=1`，主机窗口 `class=chast_cursor_hint rect=1000,547 32x32` 可见；覆盖层显示与隐藏两张全屏截图中，窗口四角、左右边缘和窗口外采样点逐像素相同（`16,17,14`，maxdiff=0），仅箭头填充采样从 `16,17,14` 变为 `236,236,236`；离开视频区后 `showCursor ... vis=0` 且窗口不可见。放大裁切 `build/gui-ptr-fix-show-crop.png` 只有箭头、无方块。实现见 [src/win/show_cursor.cc:219](../../src/win/show_cursor.cc)。 |
| 隐藏指针语义 | 代码路径完整，OS 级真实场景未验收 | 仅查看：`CursorPublisher` 发送 `visible=false`，Dart 叠层立即置空；控制模式：保留最后形状。RustDesk 用 `GetCursorInfo` + `CURSOR_SHOWING` 判定隐藏（`D:/rustdesk/src/platform/windows.rs:199`），我们走 DXGI `PointerPosition.Visible`。本机无法稳定制造“前台窗口隐藏光标”的场景：`ShowCursor` 按线程计数，helper 窗口无法代替其他进程的光标状态，且本机指针被外部操作持续移动；为避免干扰已停止测试窗口。 |
| 512 px 光标 | 源码与 GDI 真实验收通过；DXGI 真实形状待环境 | 主机同时读取 `HKCU\Control Panel\Cursors\CursorBaseSize` 和 `HKCU\Software\Microsoft\Accessibility\CursorSize`，取两者较大的有效档位，再按当前显示器有效 DPI 换算为物理像素。GDI 与 DXGI 共用缩放、热点映射、反色掩码和描边派生函数，发布精灵上限为 512 px。2026-11-14 临时切换系统档位后，真实 GDI 桌面光标在 1/13/15 档分别为 32/224/256 px，形状均有效且可见，内容 ID 各不相同；设置已恢复。当前桌面会话的 DXGI 采集连续 30 次未取得 pointer shape，因此真实 DXGI 大光标和查看端叠层的跨进程目视仍未验收。 |

## 2. 本轮相关改动

- 采集：`pump_cursor` 忽略无鼠标更新的帧，形状查询也只在真实鼠标更新帧执行。
- Windows 指针尺寸：辅助功能 1～15 档映射为 `32 + (level - 1) * 16`，即 32～256 px；该值再乘显示器 `MDT_EFFECTIVE_DPI / 96`。若两个注册表表示不一致，取较大的有效档位，避免系统设置或测试只更新一个键时退回 32 px。
- 主机精灵派生：GDI 保留最多 2048 px 的源位图，DXGI 保存未缩放的 decode；二者统一经 `derive_cursor_sprite` 等比缩放到目标尺寸、映射热点、缩放预乘 BGRA 与反色掩码，并在需要时加入两像素描边。最终精灵不超过 512 px。
- DXGI 缓存更新：即使驱动以零字节表示“shape unchanged”，`CursorDecoder` 也会在辅助功能档位或显示器 DPI 改变时从缓存的未缩放形状重新派生，避免设置变化后继续发布旧尺寸。
- 查看端原生：`DrainCursorUpdates` 维护“控制路径的最新状态”和“仅查看路径的最新 sprite + 版本”，`takeRemoteCursor` 只在形状变化时附带 BGRA；精灵上限 512 px。
- 查看端 Dart：仅查看时启动 33 ms 光标轮询并在画面叠层绘制；进入控制后停止轮询，交给原生光标。主会话与独立显示器窗口都接入了“显示我的光标”的 33 ms 节流与关闭/离开时隐藏。
- 主机：`CursorHintOverlay` 独立线程、点击穿透、不激活、置顶；坐标不进入注入器。覆盖层改为在工作线程创建 top-down 32bpp DIB，自绘白填充/黑描边抗锯齿箭头，`move()` 用 `UpdateLayeredWindow(..., ULW_ALPHA)` 提交逐像素 BGRA；删除 `SetLayeredWindowAttributes`、`WM_PAINT` 与 `InvalidateRect`，虚拟桌面坐标钳制不变。
- 临时 stderr 诊断已从 `viewer_bridge.cc` 的 `showCursor` 分支移除；下一次 Release 重建即得到干净二进制（当前 `build/` 中的查看端仍是带诊断的 16:56 构建，仅本机验收用）。

## 3. 缺陷与下一步

1. ~~`CursorHintOverlay` 透明化~~ 已完成（2026-11-11）：改用 `UpdateLayeredWindow` 的逐像素 BGRA 自绘箭头，重建 `chast-host` 后以真实查看端回环复核通过，覆盖层显示/隐藏的前后截图像素对比见第 1 节。
2. ~~512 px：核对主机 sprite 尺寸~~ GDI 部分已完成（2026-11-14）：临时设置辅助功能 1/13/15 档并调用 `SPI_SETCURSORS` 后，`chast-cursor-test --real-gdi` 分别得到：

   | 档位 | CursorBaseSize | 目标尺寸 | 真实桌面形状 | 结果 |
   | ---: | ---: | ---: | --- | --- |
   | 1 | 32 | 32 | `32x32`、有效、可见 | 通过 |
   | 13 | 224 | 224 | `224x224`、有效、可见 | 通过 |
   | 15 | 256 | 256 | `256x256`、有效、可见 | 通过 |

   三档内容 ID 各不相同。`--real-gdi` 已收紧为真实验收断言：形状必须有效、可见，最大边必须达到按设置派生的目标尺寸，并允许反色描边额外增加两像素；只满足“≤512 px”不再算通过。测试脚本在 `finally` 中恢复 `CursorSize=1`、`CursorBaseSize=32`、`CursorType=0` 并再次调用 `SPI_SETCURSORS`；随后读回设置确认已恢复。

   DXGI 仍需在真实交互式桌面会话执行 `chast-cursor-test --real`。本次在 15 档连续 30 次采集均为 `result=0 shape=0`，没有致命采集错误，但当前会话无法提供 pointer shape；因此还不能声称 DXGI 真实大光标或查看端 hotspot 对齐通过。
3. 隐藏指针：用一个真正获得焦点的前台窗口在 `WM_SETCURSOR` 中隐藏光标，分别验证仅查看叠层消失和控制模式保留最后形状；若本机无法完成，移交双机环境。
4. ~~RustDesk 式白板（彩色半透明光标 + 用户名 + 点击涟漪）~~ 已完成（2026-11-11，v71）：`ShowCursor` 增加按下边沿，主机覆盖层按设备名取色、绘制姓名并播放 420 ms 波纹；逐像素光栅抽成 `src/win/cursor_hint_paint.cc` 并由 `cursor_hint` 测试覆盖。自由绘制标注仍未实现。
5. 修完 1–2 后重建干净查看端，跑一次主会话 + 独立显示器窗口冒烟，再关闭 #340。

## 4. 边界

- 单机回环（主机 UDP 4611、本机查看端），1 个 1080p 输出、100% 缩放；辅助功能倍率已做真实 GDI 三档采集，但未覆盖混合 DPI、负坐标、多屏和 Windows 10。
- 本机指针被外部操作，位置类断言以截图与主机窗口枚举为主，未做自动化坐标回归。
- 协议 v71 的 `ShowCursor` 带 x/y/visible/按下边沿；查看端指针坐标不进入注入路径。
- 颜色、姓名标签与 420 ms 波纹目前只有 `cursor_hint` 纯光栅测试与查看端按下沿用例；本机没有可控点击和双机环境，覆盖层的标签/波纹观感与多屏位置尚未做真实回环目视。
- 真实 GDI 三档证明主机能从系统设置派生并采集 32/224/256 px；DXGI 的缩放与缓存派生由纯逻辑测试、Release 构建和现有采集回归覆盖，但当前会话没有真实 pointer shape，不能外推为 DXGI 端到端完成。
