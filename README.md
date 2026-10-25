# StarZip 0.0.2

StarZip 是一个基于 **C++17 + Qt 6 Widgets + libarchive** 的 Windows 优先桌面压缩/解压工具。
0.0.2 在 0.0.1 的基础上补齐了压缩能力与密码支持，并把界面重做成接近 WinRAR / 7-Zip
的常规桌面工具：菜单栏、工具栏、路径卡片、压缩包树形列表、压缩设置与任务日志。

所有压缩/解压/测试操作都在后台 `QThread` 中执行，UI 不阻塞，可随时取消。

## 0.0.2 功能

### 打开与浏览

- 拖拽压缩包到窗口任意位置打开
- “打开压缩包…”文件对话框，或启动参数 `StarZip.exe <压缩包路径>`
- 自动识别 ZIP、7Z、RAR / RAR5、TAR、GZ、TGZ、BZ2、XZ、LZMA、CAB、ISO 等格式
- 树形条目列表（名称、大小、类型、修改时间、加密标记），文件夹可展开/折叠
- 按名称实时筛选
- 最近文件菜单（最多 8 条，`QSettings` 持久化）
- 状态栏显示格式、条目数、解压后大小与任务状态

### 解压与校验

- “解压到此处”解压到路径卡片中的目标目录（默认启动目录）
- “解压到…”选择目录后立即解压
- 选择性解压：在树中选中文件/文件夹后“解压选中项…”
- “测试压缩包”完整读取数据并校验 CRC，报告是否损坏
- 读取加密 ZIP / 7Z / RAR 时弹出密码框，最多重试 3 次
- 上下文菜单：复制条目路径、解压选中项、测试压缩包
- 完成后提示结果，可直接打开输出文件夹

### 压缩

- 输出格式：**ZIP、7Z、TAR、TAR.GZ、TAR.XZ**
- 可添加多个文件或整个文件夹，支持移除/清空待压缩列表
- 压缩级别 1–9
- ZIP 密码：优先 **AES-256**；运行库不支持时自动回退 **ZipCrypto**
- 输出路径留空时按格式与当前时间自动生成；可手动浏览指定
- 64 KiB 分块流式读写，不把整包读入内存
- 7Z 使用 LZMA2；TAR 系列适合跨平台归档

### 安全与可靠性

- 路径穿越防护：拒绝绝对路径、盘符、`..`、NTFS 数据流等不安全条目
- 符号链接/特殊文件跳过并提示，不跟随链接写出目标目录
- 后台任务支持取消；关闭窗口前会先请求取消并等待线程结束
- libarchive 错误码 + 中文解释，弹窗与状态栏双通道提示
- 不支持的功能显式报错，不静默失败

## 格式支持矩阵

| 格式 | 读取/解压 | 创建 | 创建时密码 |
| --- | --- | --- | --- |
| ZIP | ✅ | ✅ | ✅ AES-256 / ZipCrypto |
| 7Z | ✅ | ✅ | ❌（UI 明确提示改用 ZIP） |
| RAR / RAR5 | ✅ | ❌ | 仅读取时支持密码 |
| TAR | ✅ | ✅ | ❌ |
| TAR.GZ / TGZ | ✅ | ✅ | ❌ |
| TAR.XZ | ✅ | ✅ | ❌ |
| GZ / BZ2 / XZ / LZMA / CAB / ISO | ✅ | ❌ | 视格式而定 |

明确不在 0.0.2 范围：分卷、修复、格式互转、右键菜单集成、RAR 创建、主题切换、
完整命令行接口（仅支持启动时打开一个压缩包）。

## 从源码构建

依赖：

- CMake ≥ 3.20
- C++17 编译器（Windows 推荐 MinGW-w64 13.1 或 MSVC）
- Qt 6.3+（Widgets）
- libarchive 3.6+（需要启用 zlib / lzma；ZIP AES 由 libarchive 的加密后端提供）

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=C:/path/to/Qt/6.9.3/mingw_64;C:/path/to/libarchive" `
  -DSTARZIP_BUILD_TESTS=ON
cmake --build build
```

运行自检（需要把 libarchive 与 Qt 的 `bin` 加入 `PATH`）：

```powershell
$env:PATH = "C:\path\to\libarchive\bin;$env:PATH"
.\build\StarZipSelfTest.exe .\tests\samples\sample.7z `
  .\tests\samples\test_read_format_rar_compress_normal.rar `
  .\tests\samples\test_read_format_rar_unicode.rar `
  .\tests\samples\test_read_format_rar5_multiple_files.rar `
  .\tests\samples\test_read_format_rar5_stored.rar
```

自检覆盖列表、解压、压缩（ZIP / 7Z / TAR / TAR.GZ / TAR.XZ）、ZIP AES-256 密码、
7Z 中文路径往返、选择性解压、CRC 测试、Zip Slip 防护与 RAR/RAR5 读取。
0.0.2 当前结果：**97 项检查全部通过**。

### 打包

```powershell
cmake --install build --prefix "<绝对路径>/dist/StarZip-0.0.2-win64"
```

`bin/` 中需要同时包含 `libarchive.dll`、`liblzma.dll`、`libzlib.dll`。二进制发行包
不再提交到 Git 仓库，请以本地打包或自行构建为准。

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
├─ main.cpp                         # 入口；支持 StarZip.exe <压缩包>
├─ MainWindow.h / MainWindow.cpp    # UI、交互、后台任务调度
├─ ArchiveWorker.h / ArchiveWorker.cpp  # libarchive 读写、密码、测试、取消
├─ libarchive-7z-utf8.patch         # 7Z 中文文件名修复
├─ vcpkg.json
├─ .gitignore
└─ tests/
   ├─ selftest.cpp                  # 无 GUI 核心自检
   └─ samples/                      # 7Z / RAR / RAR5 / Zip Slip 测试包
```

## 版本历史

- **0.0.2**：压缩格式扩展（ZIP/7Z/TAR/TAR.GZ/TAR.XZ）、ZIP 密码、密码解压重试、
  选择性解压、CRC 测试、最近文件、筛选、启动参数、全新专业 UI。
- **0.0.1**：拖拽/对话框打开压缩包，列表与解压，ZIP 压缩，后台任务与取消。
