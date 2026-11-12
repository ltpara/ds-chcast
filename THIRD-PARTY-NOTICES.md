# 第三方声明与许可

本文件记录随 chast 分发或由 chast 工具获取的第三方组件。其他第三方组件
（包括 MsQuic、Opus、Flutter 及其依赖）按各自发行树中的许可文件处理。

## Amyuni usbmmidd_v2（可选虚拟显示驱动）

- 组件：USB Mobile Monitor Virtual Display（`usbmmidd_v2`）
- 版权：Amyuni Technologies（具体版权声明以随包 `License.txt` 为准）
- 产品页：<https://www.amyuni.com/en/desktop-editions/usb-mobile-monitor/learn-more>
- 许可：随包提供的 `License.txt`；本仓库当前不包含许可全文
- 版本：2.0.0.1（INF `DriverVer = 08/31/2021,2.0.0.1`）
- 来源：<https://www.amyuni.com/downloads/usbmmidd_v2.zip>
- 2026-11-03 复核的安装包：199,309 字节，SHA-256
  `629b51e9944762bae73948171c65d09a79595cf4c771a82ebc003fbba5b24f51`
- 签名链：Microsoft Root Certificate Authority 2010 → Microsoft Windows
  Third Party Component CA 2014 → Microsoft Windows Hardware Compatibility
  Publisher，时间戳 2021-09-09（WHQL）；`signtool verify /pa /v` 覆盖
  `usbmmIdd.cat` 与 `x64/usbmmIdd.dll`
- 逐文件哈希与选型依据：`docs/plat-01-virtual-display-2026-11-03.md` §3.1

### 分发状态

- 仓库不提交第三方二进制。`tools/fetch-usbmmidd.ps1` 已固定上述 SHA-256
  （2026-11-06 复下载一致），下载并校验安装包后解包到
  `build/deps/usbmmidd_v2`（`build/` 已被 `.gitignore` 忽略），并生成
  `manifest.sha256` 与 `SOURCE.txt`。
- 再分发许可仍未确认：该固定哈希目前只用于内部验收测试，发布包仍不得包含
  驱动二进制。
- `tools/package-windows.ps1 -VirtualDisplayPackageDir <目录>` 或仓库
  `drivers\usbmmidd_v2` 存在时才把驱动打进安装包；默认安装包不含该驱动。
- 再分发许可与广告提示条款尚未获得 Amyuni 确认。确认前发布包不应包含该
  驱动二进制，只保留“用户主动获取 + 哈希校验 + 明确确认”的引导安装。
- 分发该驱动时必须原样保留 `License.txt`，并在安装目录附带 `SOURCE.txt`
  （下载 URL、日期、SHA-256）。
- 待办：Amyuni 确认后再在本文件追加许可全文或官方许可链接。

## libvpx（VP9 软件编解码）

- vcpkg 版本：1.16.0#1，triplet `x64-windows-static-md`，静态链接。
- 以下原样保留该包 `share/libvpx/copyright` 的声明：

```text
Copyright (c) 2010, The WebM Project authors. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.

  * Neither the name of Google, nor the WebM Project, nor the names
    of its contributors may be used to endorse or promote products
    derived from this software without specific prior written
    permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## libyuv（I444 与 BGRA 色彩转换）

- vcpkg 版本：1916，triplet `x64-windows-static-md`，静态链接。
- 以下原样保留该包 `share/libyuv/copyright` 的声明：

```text
Copyright 2011 The LibYuv Project Authors. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.

  * Neither the name of Google nor the names of its contributors may
    be used to endorse or promote products derived from this software
    without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## libjpeg-turbo（libyuv 构建依赖）

- vcpkg 版本：3.1.4.1，triplet `x64-windows-static-md`，静态链接。
- 以下原样保留该包 `share/libjpeg-turbo/copyright` 的声明：

```text
libjpeg-turbo Licenses
======================

libjpeg-turbo is covered by two compatible BSD-style open source licenses:

- The IJG (Independent JPEG Group) License, which is listed in
  [README.ijg](README.ijg)

  This license applies to the libjpeg API library and associated programs,
  including any code inherited from libjpeg and any modifications to that
  code.  Note that the libjpeg-turbo SIMD source code bears the
  [zlib License](https://opensource.org/licenses/Zlib), but in the context of
  the overall libjpeg API library, the terms of the zlib License are subsumed
  by the terms of the IJG License.

- The Modified (3-clause) BSD License, which is listed below

  This license applies to the TurboJPEG API library and associated programs, as
  well as the build system.  Note that the TurboJPEG API library wraps the
  libjpeg API library, so in the context of the overall TurboJPEG API library,
  both the terms of the IJG License and the terms of the Modified (3-clause)
  BSD License apply.


Complying with the libjpeg-turbo Licenses
=========================================

This section provides a roll-up of the libjpeg-turbo licensing terms, to the
best of our understanding.  This is not a license in and of itself.  It is
intended solely for clarification.

1.  If you are distributing a modified version of the libjpeg-turbo source,
    then:

    1.  You cannot alter or remove any existing copyright or license notices
        from the source.

        **Origin**
        - Clause 1 of the IJG License
        - Clause 1 of the Modified BSD License
        - Clauses 1 and 3 of the zlib License

    2.  You must add your own copyright notice to the header of each source
        file you modified, so others can tell that you modified that file.  (If
        there is not an existing copyright header in that file, then you can
        simply add a notice stating that you modified the file.)

        **Origin**
        - Clause 1 of the IJG License
        - Clause 2 of the zlib License

    3.  You must include the IJG README file, and you must not alter any of the
        copyright or license text in that file.

        **Origin**
        - Clause 1 of the IJG License

2.  If you are distributing only libjpeg-turbo binaries without the source, or
    if you are distributing an application that statically links with
    libjpeg-turbo, then:

    1.  Your product documentation must include a message stating:

        This software is based in part on the work of the Independent JPEG
        Group.

        **Origin**
        - Clause 2 of the IJG license

    2.  If your binary distribution includes or uses the TurboJPEG API, then
        your product documentation must include the text of the Modified BSD
        License (see below.)

        **Origin**
        - Clause 2 of the Modified BSD License

3.  You cannot use the name of the IJG or The libjpeg-turbo Project or the
    contributors thereof in advertising, publicity, etc.

    **Origin**
    - IJG License
    - Clause 3 of the Modified BSD License

4.  The IJG and The libjpeg-turbo Project do not warrant libjpeg-turbo to be
    free of defects, nor do we accept any liability for undesirable
    consequences resulting from your use of the software.

    **Origin**
    - IJG License
    - Modified BSD License
    - zlib License


The Modified (3-clause) BSD License
===================================

Copyright (C) 2009-2026 D. R. Commander.  All Rights Reserved.<br>
Copyright (C) 2015 Viktor Szathmáry.  All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
- Neither the name of the libjpeg-turbo Project nor the names of its
  contributors may be used to endorse or promote products derived from this
  software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS",
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.


Why Two Licenses?
=================

The zlib License could have been used instead of the Modified (3-clause) BSD
License, and since the IJG License effectively subsumes the distribution
conditions of the zlib License, this would have effectively placed
libjpeg-turbo binary distributions under the IJG License.  However, the IJG
License specifically refers to the Independent JPEG Group and does not extend
attribution and endorsement protections to other entities.  Thus, it was
desirable to choose a license that granted us the same protections for new code
that were granted to the IJG for code derived from their software.
```
