# 文件浏览目录上传（协议 v74，2026-11-13）

## 1. 结论

协议 **v74** 为查看端到主机的文件上传增加目录 offer。拖入的文件夹现会保留
自身及完整子目录结构，包括独立空目录、嵌套空目录和混合目录树；普通文件及
零字节普通文件仍按文件传输。ViewerEngine 专用真实 QUIC 回环、`chast-quic
verify --file-upload`、协议测试、文件传输测试和协议模糊测试均通过。

本轮关闭的是目录上传的代码、协议和单机自动化验收。Flutter 文件面板的真实
拖放人工操作、双机网络和跨平台主机仍未验收。

## 2. 协议契约

`chast::FileOffer` 新增 `directory` 字段，wire 标志为
`kFileOfferFlagDirectory = 0x04`。目录 offer 必须满足：

- `size == 0`；
- `modified_time == 0`，SHA-256 全为零；
- 不能带 `resume` 或 `clipboard` 标志；
- `name` 是单个安全文件名，`relative_path` 仍是 `/` 分隔的纯相对目录；
- 目录没有 CFIL 文件流，也不消耗文件字节。

主机收到目录 offer 后创建目标目录，再发送 `FileAnswer(Ok)` 和
`FileResult(Ok)`。重复创建同一目录是幂等的；与普通文件同名时仍走 v69
冲突处理并返回明确原因。协议层和 QUIC 发送层都拒绝为目录调用文件数据或
完成流接口；如果攻击者仍请求 `CFIL`，接收层以
“a directory offer has no file stream”拒绝。

## 3. ViewerEngine 行为

- `SendFiles()` 展开每个选中的目录，先发送目录自身，再发送后代；空目录不会
  因没有文件而消失。
- 目录以 `kReady` 进入队列，不计算 SHA-256，也不打开文件流。
- 成功目录不增加 `files_sent`、`files_skipped` 或文件审计条目；失败目录增加
  `file_failures`，但仍不进入普通文件审计。
- 普通文件包括零字节文件，正常增加 `files_sent` 并生成文件审计条目。
- 上传根和目录遍历均使用 Win32 `FILE_ATTRIBUTE_REPARSE_POINT` 检查。目录
  junction 作为上传根时明确失败；位于普通树内时被跳过，既不递归到目标，也
  不创建目标侧空目录。

## 4. 主机边界

主机创建目录前继续使用现有共享根路径校验：拒绝绝对路径、盘符、`.`、`..`、
空组件、过长路径和过深嵌套。目录目标必须保持在配置的接收目录或共享根内。
创建过程不跟随源端链接；Windows reparse point 的排除在查看端展开上传树时
完成。

## 5. 自动化证据

### 5.1 ViewerEngine 专用探针

`chast-viewer-probe --directory-upload` 使用真实 ViewerEngine、真实
HostServer 和本地 QUIC，覆盖：

- 1 个独立空目录、3 个嵌套空目录、5 个混合树目录，共 9 个目录；
- 2 个带内容文件、1 个零字节文件，共 3 个普通文件；
- 文件字节逐项比对，零字节文件必须仍是普通文件；
- 9 个目录全部落盘，但 `files_sent` 只增加 3，文件审计也恰好只有 3 项；
- 目录 junction 作为上传根时产生 1 次失败和明确原因，不创建目标项，也不进入
  文件审计；
- junction 位于混合树内时被跳过，外部标记文件未上传，树内普通文件和目录正常。

该测试注册为 `viewer_directory_upload`，超时 180 秒。本次 Release 构建通过，
CTest 结果为 `1/1 Passed`。

### 5.2 真实 QUIC 验证

`chast-quic verify --file-upload` 本次通过，覆盖 12 个文件/目录 offer：

- 普通上传、截断续传和相对目录上传；
- 摘要错误拒绝和文件名冲突拒绝；
- 顶层空目录、嵌套空目录和重复创建；
- 混合目录、零字节文件和文件占用目录名冲突；
- 上传端无协议错误，主机只对 4 个真实文件完成计数，目录不改变文件计数；
- 全部目录和文件落盘断言通过；
- 同轮音视频、剪贴板、光标、输入和离线回放检查也全部通过。

### 5.3 定向测试

- `chast-protocol-test`：通过；包含目录标志、目录字段约束、非法流和裁剪输入。
- `chast-file-transfer-test`：通过。
- `chast-protocol-fuzz-test`：80 个解析入口、每个 4000 次变异（流帧 2000 次）
  通过。
- 构建目标：`chast-viewer-probe`、`chast-quic`、`chast-host`、
  `chast-file-transfer-test`、`chast-protocol-test`、`chast-protocol-fuzz-test`
  的 Release 构建通过。

本机执行环境中的探针子进程不能写入用户 `%TEMP%` 和 Downloads 隔离目录。
目录探针因此会先尝试系统临时目录，失败后回退到当前构建树下的唯一
`test-temp` 目录；协议和文件传输测试以工作区临时目录作为 `TEMP/TMP`
复验通过。这是本机运行权限边界，不是协议功能失败。

## 6. 剩余验收

- 在 Flutter 文件面板中人工拖入空目录、多层目录、混合目录和零字节文件；
- 双机、弱网、断线续传和不同盘符/共享根组合；
- macOS/Linux 主机对目录 offer 的等价实现；
- 真实跨卷 junction/符号链接策略和需要提升权限的链接类型。

在上述环境验收完成前，目录上传可记为“代码完成、单机自动化通过”，不能记为
全平台或跨机验收完成。
