# StarZip 0.1.0

StarZip 是一个使用 **C++17 + Qt 6 Widgets + libarchive** 构建的 Windows 压缩/解压工具。
0.1.0 的核心新增是作者自有的 **ZPack（.stz）** 单文件压缩格式：
分块自适应 + LZ77 + 规范 Huffman + 位级打包 + 存储回退 + 每块 CRC32。

StarZip 同时保留了 0.0.3 已有的 Windows 右键菜单、UDF ISO 完整目录树、加密 7Z、
ZIP AES-256、选择性解压、CRC 测试和紧凑窗口。

## 0.1.0 新增

### ZPack（.stz）

- 官方格式：魔数 `STZ1`，版本 1，文件头记录原始大小与块数。
- 智能分块：64 KiB 起，按 64 KiB 窗口字节频率的曼哈顿距离检测分布突变，
  最迟 1 MiB 强制切块。
- LZ77：32 KiB 滑动窗口、最小匹配 3 字节、最大 258 字节、哈希链加速、
  一步惰性匹配。
- 规范 Huffman：0–255 字面量、256 块结束、257–285 长度码、286–315 距离码；
  只保存每个符号的码长，解码端重建规范码表。
- 位级打包：码字写入连续 bit 流，记录尾部有效 bit 数。
- 存储回退：压缩后不小于原块时整块存储，随机数据与 JPEG/ZIP 等已压缩文件
  不会膨胀超过少量块头开销。
- 完整性：每块 CRC32；解压时逐块校验，损坏包不留下半成品输出。
- 安全边界：拒绝过大的块字段、无效码长、越界回溯距离、输出超出块大小、
  文件尾多余数据等畸形输入。

ZPack 是**单文件**格式：创建 `.stz` 时只能选择一个普通文件，不支持文件夹、
多来源或密码；多文件/加密场景请使用 ZIP / 7Z / TAR。

### ZPack 文件布局

```text
文件头（17 字节）
  "STZ1"        4
  version       1
  originalSize  8  little-endian
  blockCount    4  little-endian

每个块
  type          1  (0 存储 / 1 纯 Huffman / 2 LZ77+Huffman)
  rawSize       4
  compressedSize 4
  crc32         4
  tableSize     2
  codeLengths   tableSize
  validBits     1
  payload       compressedSize
```

### ZPack 实测（本机 0.1.0 自检语料）

| 语料 | 原始大小 | 压缩后 | 比率 |
| --- | ---: | ---: | ---: |
| 重复文本 | 378,320 | 9,635 | 2.55% |
| 源码拼接 | 855,208 | 186,283 | 21.78% |
| 结构化日志 | 10,027,705 | 1,595,140 | 15.91% |
| 混合大文件（跨块） | 4,292,565 | 550,350 | 12.82% |
| 全相同字节 | 4,194,304 | 17,609 | 0.42% |
| 随机二进制 | 4,194,304 | 4,194,385 | 100.002%（存储回退） |
| 已压缩 7Z | 1,189 | 1,222 | 102.8%（存储回退） |

ZPack 的压缩率属于 DEFLATE 级别，不追求击败 7z / zstd；目标是可控、可审计、
带每块校验的 StarZip 自有格式。

## 通用功能

### 打开与浏览

- 拖拽压缩包到窗口，或通过文件对话框打开，支持 `StarZip.exe <压缩包路径>`。
- 自动识别 ZIP、7Z、RAR / RAR5、TAR、GZ、TGZ、BZ2、XZ、LZMA、CAB、ISO、STZ。
- **UDF ISO**：识别 `NSR02/NSR03`，通过内置 7-Zip 引擎列出完整安装盘目录。
- 条目列表包含名称、大小、类型、修改时间、加密标记；目录可展开/折叠。
- 列表支持实时筛选、右键菜单、状态栏显示格式/条目数/解压后大小。
- 默认窗口 900×600，最小 760×500。

### 解压与校验

- 解压到压缩包所在目录、指定目录或当前目录。
- 支持按选中文件/文件夹进行选择性解压。
- 解压完成后可核对条目数、文件数、跳过数。
- 支持“测试压缩包”完整读取并校验 CRC/损坏情况。
- 加密 ZIP / 7Z / RAR 可输入密码；密码最多重试 3 次。
- 加密 7Z 的列表、解压、测试交给内置 7-Zip 引擎。
- 路径穿越防护：拒绝绝对路径、盘符、`..` 与 NTFS 特殊流等不安全条目。
- 解压取消后等待后台线程结束再退出，避免残留线程。
- ZPack 解压逐块 CRC 校验，失败时删除临时输出。

### 压缩

- 输出格式：**ZIP、7Z、TAR、TAR.GZ、TAR.XZ、ZPack（.stz）**。
- 可添加多个文件/文件夹，支持移除、清空压缩列表。
- 压缩级别 0–9；ZPack 当前不需要级别（固定自适应策略）。
- ZIP 密码：优先 **AES-256**，运行库不支持时自动回退 **ZipCrypto**。
- 7Z 密码：由 7-Zip 引擎写入（AES-256）；缺少引擎时 UI 明确提示。
- 输出路径留空时按格式与当前时间自动生成，也可手动指定。
- ZIP / 7Z / TAR 使用 64 KiB 分块流式写入，控制大文件内存占用。
- ZPack 仅允许单文件，多来源/文件夹/密码会被明确拒绝。

### Windows 资源管理器集成

首次运行自动为**当前用户**写入右键菜单，无需管理员权限；
设置(&T)菜单中可随时安装/修复或移除。

- 压缩包（`.zip .7z .stz .rar .tar .gz .tgz .bz2 .xz .lzma .cab .iso`）：
  - 使用 StarZip 打开
  - 解压到当前文件夹（解压到压缩包同名子目录）
  - 测试压缩包
- 普通文件 / 文件夹：
  - 使用 StarZip 压缩
- 同时注册“打开方式”中的 StarZip 项目。

Windows 11 会把传统注册表右键菜单收进“显示更多选项”；
StarZip 的菜单在经典菜单中始终可见。

### 命令行

```text
StarZip.exe "archive.zip"                  # 打开压缩包
StarZip.exe --extract-here "archive.zip"   # 解压到压缩包旁同名目录
StarZip.exe --extract-to "archive.zip"     # 选择目录后解压
StarZip.exe --test "archive.zip"           # 打开并自动测试
StarZip.exe --compress "file-or-folder"    # 打开压缩页并加入来源
```

### 安全与可靠性

- 路径穿越、绝对路径、盘符与 NTFS 特殊流条目一律拒绝。
- 符号链接/特殊文件不直接写入目标目录。
- libarchive 错误码 + 中文解释；外部 7-Zip 输出通过 UTF-8 捕获。
- 未知/不支持的格式给出明确错误，而不是静默失败。
- 外部 7-Zip 仅用于 UDF ISO 与加密 7Z，不改变其他格式的 libarchive 路径。
- ZPack 写入使用临时文件 + 原子重命名，失败不会留下半截 `.stz`。

## 格式支持矩阵

| 格式 | 读取/解压 | 创建 | 密码 |
| --- | :---: | :---: | --- |
| ZIP | ✅ | ✅ | ✅ AES-256 / ZipCrypto |
| 7Z | ✅ | ✅ | ✅ 7-Zip 引擎（AES-256） |
| RAR / RAR5 | ✅ | ❌ | 仅读取时支持密码 |
| TAR | ✅ | ✅ | ❌ |
| TAR.GZ / TGZ | ✅ | ✅ | ❌ |
| TAR.XZ | ✅ | ✅ | ❌ |
| GZ / BZ2 / XZ / LZMA / CAB | ✅ | ❌ | 仅读取 |
| ISO / UDF ISO | ✅ | ❌ | ❌ |
| ZPack（.stz） | ✅ | ✅ | ❌ |

## 已知限制

以下是当前版本的真实边界，遇到时会给出明确提示，而不是静默失败：

- **创建 RAR**：RAR 是专有格式，libarchive / 7-Zip 都不能写入 RAR；
  需要 RARLAB 专有 SDK，本版本不支持。
- **UDF ISO 依赖 7-Zip 引擎**：仓库内包含 `7z.exe` / `7z.dll`；
  删除后 UDF ISO 将退回显示兼容层目录。
- **ZPack 仍是单文件算法**：没有多文件容器、加密、分卷或固实块；
  `.stz` 内只有一条文件流。
- **ZPack 压缩率/速度**：DEFLATE 级别，低于 7z / zstd；取消检查在块/词法
  阶段，极端大的单块解码中响应不如流式算法细。
- **分卷/修复/格式转换/主题切换**：尚未实现。
- **RAR 新压缩算法**：可解压程度取决于 libarchive 版本。
- Windows 11 现代右键菜单需要展开“显示更多选项”。

## 源码与构建

依赖：

- CMake ≥ 3.20
- C++17（Windows 推荐 MinGW-w64 13.1 或 MSVC）
- Qt 6.3+（Widgets）
- libarchive 3.6+（需要 zlib / lzma；ZIP AES 依赖 libarchive 加密后端）
- 仓库内 `third_party/7zip/` 已包含 `7z.exe`、`7z.dll`、`License.txt`（7-Zip 24.09）

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=C:/path/to/Qt/6.9.3/mingw_64;C:/path/to/libarchive" `
  -DSTARZIP_BUILD_TESTS=ON
cmake --build build
```

运行自检（需要 libarchive 与 Qt 的 `bin` 在 `PATH`）：

```powershell
$env:PATH = "C:\path\to\libarchive\bin;C:\path\to\Qt\bin;$env:PATH"
.\build\StarZipSelfTest.exe
```

可选：使用真实 UDF ISO 验证目录树：

```powershell
$env:STARZIP_UDF_ISO = "D:\Win11_24H2_Chinese_Simplified_x64.iso"
.\build\StarZipSelfTest.exe
```

可选：验证右键菜单安装/移除（会写入当前用户注册表）：

```powershell
$env:STARZIP_TEST_SHELL = "1"
$env:STARZIP_TEST_SHELL_UNINSTALL = "1"
.\build\StarZipSelfTest.exe
```

自检覆盖：ZIP / 7Z / TAR / TAR.GZ / TAR.XZ 创建、列表、解压与 CRC，
ZIP / 7Z 密码、错误密码、选择性解压、Zip Slip 防护、RAR/RAR5 读取、
UDF ISO 目录树、右键菜单注册，以及 ZPack 的空文件、1/2 字节、全相同、
单符号、文本、代码、日志、随机、已压缩、跨块大文件往返与 CRC 损坏检测。
0.1.0 当前结果：**全部通过**。

### ZPack 调试/基准工具

构建目标 `ZPackTool`：

```powershell
.\build\ZPackTool.exe compress input.bin output.stz
.\build\ZPackTool.exe decompress output.stz restored.bin
.\build\ZPackTool.exe test output.stz
.\build\ZPackTool.exe info output.stz
.\build\ZPackTool.exe roundtrip input.bin
```

### 部署

```powershell
cmake --install build --prefix "<绝对路径>/dist/StarZip-0.1.0-win64"
```

`bin/` 会包含 `StarZip.exe`、Qt 运行库、`libarchive.dll`、`liblzma.dll`、
`libzlib.dll`，以及 `7z.exe`、`7z.dll`、`License.txt`。二进制产物不提交到
Git 仓库，以本地构建或发布包为准。

## libarchive 7Z 中文文件名补丁说明

libarchive 3.8.2 在 Windows 上创建 7Z 时，路径会先经过当前 locale 转换。
中文/非 ASCII 文件名可能被转成控制字符并破坏 7Z 头，导致 7-Zip 报
`Damaged 7-Zip archive`。

仓库中的 `libarchive-7z-utf8.patch` 修改
`libarchive/archive_write_set_format_7zip.c` 的 `file_new()`，直接使用
`MultiByteToWideChar(CP_UTF8, ...)` 将 UTF-8 转为 UTF-16LE，从源头避免该问题。

```powershell
# 在 libarchive 源码目录
git apply -p1 /path/to/StarZip/libarchive-7z-utf8.patch
cmake -S . -B build-starzip -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_SHARED_LIBS=ON -DENABLE_TEST=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF `
  -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF
cmake --build build-starzip
cmake --install build-starzip --prefix C:/path/to/libarchive-install
```

## 目录结构

```text
StarZip/
├── CMakeLists.txt
├── main.cpp                         # 入口与命令行参数
├── MainWindow.h / MainWindow.cpp    # UI、任务调度、右键菜单入口
├── ArchiveWorker.h / ArchiveWorker.cpp  # 列表、解压、测试、压缩
├── ZPack.h / ZPack.cpp              # 自有 ZPack（.stz）编解码
├── ExternalEngine.h / ExternalEngine.cpp  # 7-Zip 引擎：UDF ISO、加密 7Z
├── ShellIntegration.h / ShellIntegration.cpp  # HKCU 右键菜单注册
├── third_party/7zip/                # 内置 7z.exe / 7z.dll / License.txt
├── libarchive-7z-utf8.patch         # 7Z 中文文件名修复
├── vcpkg.json
├── .gitignore
├── tests/
│   ├── selftest.cpp                 # 无 GUI 自检
│   ├── zpack_cli.cpp                # ZPack 调试/基准 CLI
│   └── samples/                     # 7Z / RAR / RAR5 / Zip Slip 测试包
```

## 版本历史

- **0.1.0**：新增 ZPack（.stz）分块自适应 LZ77 + 规范 Huffman + 位级打包 +
  存储回退 + CRC32；接入 GUI、ArchiveWorker、右键菜单与自检；新增 ZPackTool。
- **0.0.3**：Windows 右键菜单与“打开方式”集成、UDF ISO 完整目录树、内置 7-Zip
  引擎、加密 7Z 创建/解压/测试、紧凑窗口 900×600。
- **0.0.2**：压缩格式扩展（ZIP/7Z/TAR/TAR.GZ/TAR.XZ）、ZIP 密码、选择性解压、
  CRC 测试、输出文件与筛选、错误处理和更完整的 UI。
- **0.0.1**：拖拽/对话框打开、列表、解压、ZIP 压缩、命令行与基础提取。
