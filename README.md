# StarZip 0.0.3

StarZip 是一个基于 **C++17 + Qt 6 Widgets + libarchive** 的 Windows 优先桌面压缩/解压工具。
0.0.3 重点补齐了真实使用中最影响体验的三件事：

- **Windows 右键菜单**：压缩包可直接“使用 StarZip 打开 / 解压到当前文件夹 / 解压到…”，
  任意文件或文件夹可直接“使用 StarZip 压缩…”。
- **UDF ISO 真实内容**：Windows 安装盘等 UDF 光盘映像现在能列出并解压完整目录树，
  不再只显示 ISO9660 兼容层里的 `README.TXT`。
- **紧凑窗口**：默认 900×600，最小 760×500，像正常桌面软件而不是大型工具面板。

所有压缩/解压/测试操作仍在后台 `QThread` 中执行，UI 不阻塞，可随时取消。

## 0.0.3 功能

### 打开与浏览

- 拖拽压缩包到窗口任意位置打开
- “打开压缩包…”文件对话框，或启动参数 `StarZip.exe <压缩包路径>`
- 自动识别 ZIP、7Z、RAR / RAR5、TAR、GZ、TGZ、BZ2、XZ、LZMA、CAB、ISO 等格式
- **UDF ISO**：自动检测 `NSR02/NSR03`，通过随程序分发的 7-Zip 引擎列出完整目录树
- 树形条目列表（名称、大小、类型、修改时间、加密标记），文件夹可展开/折叠
- 按名称实时筛选；最近文件菜单；状态栏显示格式、条目数、解压后大小
- 窗口默认 900×600，最小 760×500

### 解压与校验

- “解压到此处”解压到路径卡片中的目标目录（默认启动目录）
- “解压到…”选择目录后立即解压
- 选择性解压：在树中选中文件/文件夹后“解压选中项…”
- “测试压缩包”完整读取数据并校验 CRC，报告是否损坏
- 读取加密 ZIP / 7Z / RAR 时弹出密码框，最多重试 3 次
- 加密 7Z 的列表/解压/测试在输入密码后交给 7-Zip 引擎处理
- 上下文菜单：复制条目路径、解压选中项、测试压缩包
- 完成后提示结果，可直接打开输出文件夹

### 压缩

- 输出格式：**ZIP、7Z、TAR、TAR.GZ、TAR.XZ**
- 可添加多个文件或整个文件夹，支持移除/清空待压缩列表
- 压缩级别 1–9
- ZIP 密码：优先 **AES-256**；运行库不支持时自动回退 **ZipCrypto**
- 7Z 密码：由 7-Zip 引擎写入（AES-256）；未找到引擎时 UI 会明确提示
- 输出路径留空时按格式与当前时间自动生成；可手动浏览指定
- 64 KiB 分块流式读写，不把整包读入内存

### Windows 资源管理器集成

首次运行会自动为**当前用户**写入右键菜单，不需要管理员权限；
“工具(&T)”菜单中可随时“安装/修复”或“移除”。

- 压缩包（`.zip .7z .rar .tar .gz .tgz .bz2 .xz .lzma .cab .iso`）：
  - 使用 StarZip 打开
  - 解压到当前文件夹（创建与压缩包同名的子目录）
  - 解压到…
- 任意文件 / 文件夹：
  - 使用 StarZip 压缩…
- 同时注册“打开方式”中的 StarZip 条目

Windows 11 的现代右键菜单可能把传统注册表菜单收进“显示更多选项”；这是系统行为，
StarZip 的菜单项在经典菜单中始终可见。

### 命令行契约（右键菜单使用）

```text
StarZip.exe "archive.zip"                  # 打开压缩包
StarZip.exe --extract-here "archive.zip"   # 解压到压缩包旁的同名目录
StarZip.exe --extract-to "archive.zip"     # 选择目录后解压
StarZip.exe --test "archive.zip"           # 打开并自动测试
StarZip.exe --compress "file-or-folder"    # 打开压缩页并加入待压缩项
```

### 安全与可靠性

- 路径穿越防护：拒绝绝对路径、盘符、`..`、NTFS 数据流等不安全条目
- 符号链接/特殊文件跳过并提示，不跟随链接写出目标目录
- 后台任务支持取消；关闭窗口前会先请求取消并等待线程结束
- libarchive 错误码 + 中文解释，弹窗与状态栏双通道提示
- 外部 7-Zip 调用使用 UTF-8 输出，进度、取消与失败清理均已处理
- 不支持的功能显式报错，不静默失败

## 格式支持矩阵

| 格式 | 读取/解压 | 创建 | 创建时密码 |
| --- | --- | --- | --- |
| ZIP | ✅ | ✅ | ✅ AES-256 / ZipCrypto |
| 7Z | ✅ | ✅ | ✅ 7-Zip 引擎（AES-256） |
| RAR / RAR5 | ✅ | ❌ | 仅读取时支持密码 |
| TAR | ✅ | ✅ | ❌ |
| TAR.GZ / TGZ | ✅ | ✅ | ❌ |
| TAR.XZ | ✅ | ✅ | ❌ |
| GZ / BZ2 / XZ / LZMA / CAB | ✅ | ❌ | 视格式而定 |
| ISO / UDF ISO | ✅ | ❌ | — |

## 已知限制

以下项目是当前版本的真实边界，遇到时会给出明确提示，而不是静默失败：

- **创建 RAR**：RAR 是专有格式，libarchive / 7-Zip 均不能写出 RAR，需要 RARLAB
  专有 SDK；本版本不支持，也不会假装支持。
- **UDF ISO 依赖 7-Zip 引擎**：发行包已内置 `7z.exe` / `7z.dll`；若被删除或杀毒软件
  隔离，UDF ISO 会提示重新安装完整 StarZip。
- **分卷压缩包、修复损坏压缩包、格式互转、主题切换**：尚未实现。
- 加密 RAR、特殊压缩算法、损坏文件的可恢复程度取决于 libarchive 版本。
- Windows 11 现代右键菜单可能需要点击“显示更多选项”。

## 从源码构建

依赖：

- CMake ≥ 3.20
- C++17 编译器（Windows 推荐 MinGW-w64 13.1 或 MSVC）
- Qt 6.3+（Widgets）
- libarchive 3.6+（需要启用 zlib / lzma；ZIP AES 由 libarchive 的加密后端提供）
- 已放入 `third_party/7zip/` 的 `7z.exe`、`7z.dll`、`License.txt`（7-Zip 24.09）

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=C:/path/to/Qt/6.9.3/mingw_64;C:/path/to/libarchive" `
  -DSTARZIP_BUILD_TESTS=ON
cmake --build build
```

运行自检（需要把 libarchive 与 Qt 的 `bin` 加入 `PATH`）：

```powershell
$env:PATH = "C:\path\to\libarchive\bin;C:\path\to\Qt\bin;$env:PATH"
.\build\StarZipSelfTest.exe
```

可选的真实 UDF ISO 检查：

```powershell
$env:STARZIP_UDF_ISO = "D:\Win11_24H2_Chinese_Simplified_x64.iso"
.\build\StarZipSelfTest.exe
```

可选地验证右键菜单安装/移除（会写入当前用户注册表）：

```powershell
$env:STARZIP_TEST_SHELL = "1"
$env:STARZIP_TEST_SHELL_UNINSTALL = "1"
.\build\StarZipSelfTest.exe
```

自检覆盖列表、解压、压缩（ZIP / 7Z / TAR / TAR.GZ / TAR.XZ）、ZIP / 7Z 密码、
7Z 中文路径往返、选择性解压、CRC 测试、Zip Slip 防护、RAR/RAR5 读取、
UDF ISO 目录树与右键菜单注册。0.0.3 当前结果：**全部通过**。

### 打包

```powershell
cmake --install build --prefix "<绝对路径>/dist/StarZip-0.0.3-win64"
```

`bin/` 中会包含 `StarZip.exe`、Qt 运行库、`libarchive.dll`、`liblzma.dll`、
`libzlib.dll`，以及 `7z.exe`、`7z.dll`、`License.txt`。二进制发行包不再提交到
Git 仓库，请以本地打包或自行构建为准。

## libarchive 7Z 中文文件名补丁（重要）

libarchive 3.8.2 在 Windows 上创建 7Z 时，会经过当前 locale 把路径转换为 UTF-16LE。
由于 Windows 的 locale 不能是 UTF-8，非 ASCII 文件名会被静默转换成空字符串，生成的
7Z 头部损坏：7-Zip 等工具可能仍能勉强读取，但 libarchive 自己会报
`Damaged 7-Zip archive`。

补丁 `libarchive-7z-utf8.patch` 在
`libarchive/archive_write_set_format_7zip.c` 的 `file_new()` 中直接调用
`MultiByteToWideChar(CP_UTF8, ...)` 完成 UTF-8 → UTF-16LE 转换，其余情况仍走原有路径。

```powershell
# 在 libarchive 源码根目录
git apply -p1 /path/to/StarZip/libarchive-7z-utf8.patch
cmake -S . -B build-starzip -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_SHARED_LIBS=ON -DENABLE_TEST=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF `
  -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF
cmake --build build-starzip
cmake --install build-starzip --prefix C:/path/to/libarchive-install
```

补丁已在 libarchive 3.8.2 上验证可干净应用并往返一致。

## 目录结构

```text
StarZip/
├─ CMakeLists.txt
├─ main.cpp                         # 入口；命令行参数解析
├─ MainWindow.h / MainWindow.cpp    # UI、交互、右键菜单管理、启动动作
├─ ArchiveWorker.h / ArchiveWorker.cpp  # libarchive 读写、密码、测试、取消
├─ ExternalEngine.h / ExternalEngine.cpp  # 7-Zip 引擎：UDF ISO、加密 7Z
├─ ShellIntegration.h / ShellIntegration.cpp  # HKCU 右键菜单注册
├─ third_party/7zip/                # 内置 7z.exe / 7z.dll / License.txt
├─ libarchive-7z-utf8.patch         # 7Z 中文文件名修复
├─ vcpkg.json
├─ .gitignore
└─ tests/
   ├─ selftest.cpp                  # 无 GUI 核心自检
   └─ samples/                      # 7Z / RAR / RAR5 / Zip Slip 测试包
```

## 版本历史

- **0.0.3**：Windows 右键菜单与“打开方式”集成、UDF ISO 完整目录树、内置 7-Zip 引擎、
  加密 7Z 创建/解压/测试、命令行参数契约、紧凑窗口（900×600）。
- **0.0.2**：压缩格式扩展（ZIP/7Z/TAR/TAR.GZ/TAR.XZ）、ZIP 密码、密码解压重试、
  选择性解压、CRC 测试、最近文件、筛选、启动参数、全新专业 UI。
- **0.0.1**：拖拽/对话框打开压缩包，列表与解压，ZIP 压缩，后台任务与取消。
