# StarZip 0.0.1

StarZip 是一个基于 **C++17 + Qt 6 Widgets + libarchive** 的 Windows 优先桌面压缩/解压工具。
0.0.1 聚焦一条高频路径：**拖入压缩包 → 解压到当前/指定目录 → 完成**，同时支持把文件或文件夹
流式压缩为 ZIP。压缩/解压全部在后台线程执行，UI 不阻塞。

## 功能清单

- 拖拽本地压缩包到窗口，libarchive 自动识别格式（ZIP、7Z、RAR/RAR5、TAR 等）
- “选择文件…”打开压缩包
- 压缩包内容列表（名称、大小、修改时间、加密标记）
- 默认解压到 StarZip 启动时的当前目录，可编辑/浏览目标目录
- “解压到此处”一键解压；“解压到…”选择目标目录并立即解压
- 选取文件/文件夹后压缩为 `.zip`（64 KiB 分块流式读写，不整包进内存）
- 实时进度条、状态栏、任务日志、取消任务
- 后台 `QThread` 执行，信号槽回传进度与结果；关闭任务中的窗口会先请求取消
- 可读错误提示：弹窗 + 状态栏双通道，包含 libarchive 错误码及中文解释
- 路径穿越防护：拒绝绝对路径、盘符、`..`、NTFS 数据流等不安全条目
- 符号链接/特殊文件跳过并提示；不跟随链接到目标目录外
- RAR/7Z 不支持项显式报错，不静默失败

明确不在 0.0.1 范围：加密/密码、分卷、修复、格式互转、右键菜单、命令行、主题切换、
RAR 专有写入、创建 RAR/7Z。

## 目录结构

```text
StarZip/
├─ CMakeLists.txt
├─ main.cpp
├─ MainWindow.h / MainWindow.cpp
├─ ArchiveWorker.h / ArchiveWorker.cpp
├─ vcpkg.json
├─ README.md
├─ .gitignore
├─ tests/
│  ├─ selftest.cpp                 # 无 GUI 核心自检程序
│  └─ samples/                     # 7Z / RAR / RAR5 / Zip Slip 测试包
└─ dist/
   └─ StarZip-0.0.1-win64/
      ├─ bin/
      │  ├─ StarZip.exe            # 可直接双击运行
      │  ├─ libarchive.dll
      │  ├─ liblzma.dll
      │  ├─ libzlib.dll
      │  ├─ Qt6*.dll               # Qt 运行库
      │  └─ qt.conf
      └─ plugins/                  # Qt 平台/图片/样式/TLS 插件
```

`MainWindow` 只负责界面、线程调度和提示；`ArchiveWorker` 封装 libarchive 的列表、解压、ZIP
压缩逻辑，所有 libarchive 句柄使用 RAII 管理。

## 依赖安装（vcpkg，完整命令）

以 PowerShell、64 位 Windows 为例：

```powershell
# 1. 安装 vcpkg（只需一次）
git clone https://github.com/microsoft/vcpkg.git M:\vcpkg
M:\vcpkg\bootstrap-vcpkg.bat

# 2. 安装 Qt6（qtbase 含 Widgets）和 libarchive
M:\vcpkg\vcpkg install qtbase:x64-windows libarchive:x64-windows

# 3. 可选：集成到用户级 CMake
M:\vcpkg\vcpkg integrate install
```

本项目带 `vcpkg.json`（manifest），也可以在项目根目录执行：

```powershell
M:\vcpkg\vcpkg install --triplet x64-windows
```

> vcpkg 会有较多依赖且 Qt6 默认从源码编译，首次安装耗时较长属正常现象。
> 若本机已安装官方 Qt 6.9.3 MinGW，也可以直接使用 Qt 的 CMake 包，见下一节。

## Windows 构建与运行

### 方案 A：使用 vcpkg（推荐给全新环境）

```powershell
cd M:\Users\Administrator\Desktop\StarZip

cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=M:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DCMAKE_PREFIX_PATH=M:/vcpkg/installed/x64-windows

cmake --build build

# 运行（构建目录）
.\build\StarZip.exe
```

如使用 Visual Studio 生成器，把 `-G Ninja` 换成 `-G "Visual Studio 17 2022" -A x64`，
并去掉 `CMAKE_BUILD_TYPE`（或使用 `--config Release`）。

### 方案 B：使用官方 Qt MinGW 预编译包 + 自编译 libarchive（本次实际验证）

本次交付实测环境：

| 组件 | 路径 / 版本 |
| --- | --- |
| Qt | `C:\StarZipDeps\Qt\6.9.3\mingw_64`（6.9.3 MinGW 64-bit） |
| MinGW | `C:\StarZipDeps\mingw1310\Tools\mingw1310_64`（GCC 13.1，与 Qt 预编译包 ABI 匹配） |
| libarchive | `C:\StarZipDeps\install`（3.8.2，含 zlib 1.3.1、xz 5.8.1，启用 RAR/RAR5/7Z 读取） |

```powershell
$env:PATH = "C:\StarZipDeps\Qt\6.9.3\mingw_64\bin;C:\StarZipDeps\mingw1310\Tools\mingw1310_64\bin;" + $env:PATH
cd M:\Users\Administrator\Desktop\StarZip

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=C:/StarZipDeps/Qt/6.9.3/mingw_64;C:/StarZipDeps/install" `
  "-DCMAKE_C_COMPILER=C:/StarZipDeps/mingw1310/Tools/mingw1310_64/bin/gcc.exe" `
  "-DCMAKE_CXX_COMPILER=C:/StarZipDeps/mingw1310/Tools/mingw1310_64/bin/g++.exe"

cmake --build build
.\build\StarZip.exe
```

> 注意：Qt 6.9.3 的 MinGW 预编译包必须搭配 GCC 13.1。使用其他 ABI 的编译器会出现
> `undefined reference to '__imp___argc'` 等链接错误。

如果 libarchive 安装在自定义目录，请把该目录加入 `CMAKE_PREFIX_PATH`，或通过
`-DLIBARCHIVE_INCLUDE_DIR=... -DLIBARCHIVE_LIBRARY=...` 指定。

### 部署到独立目录

`cmake --install` 的 prefix 必须是**绝对路径**（Qt 部署脚本要求），例如：

```powershell
cmake --install build --prefix "M:/Users/Administrator/Desktop/StarZip/dist/StarZip-0.0.1-win64"

# 若 libarchive 为动态库，补齐其运行时依赖
Copy-Item C:\StarZipDeps\install\bin\libarchive.dll,`
          C:\StarZipDeps\install\bin\liblzma.dll,`
          C:\StarZipDeps\install\bin\libzlib.dll `
          dist\StarZip-0.0.1-win64\bin\ -Force
```

`dist/StarZip-0.0.1-win64/bin/StarZip.exe` 已包含实测可运行的完整 DLL 集合，可直接双击
或在未安装 Qt 的机器上运行（需 64 位 Windows 10/11）。

## 核心自检（无 GUI）

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTARZIP_BUILD_TESTS=ON `
  "-DCMAKE_PREFIX_PATH=C:/StarZipDeps/Qt/6.9.3/mingw_64;C:/StarZipDeps/install"

cmake --build build

.\build\StarZipSelfTest.exe `
  .\tests\samples\sample.7z `
  .\tests\samples\test_read_format_rar_compress_normal.rar `
  .\tests\samples\test_read_format_rar5_multiple_files.rar `
  .\tests\samples\test_read_format_rar_unicode.rar
```

### 本次实测结果

- `RESULT: ALL PASSED`（退出码 0）
- ZIP 压缩：5 个条目，含中文路径、子目录、3 MiB 随机二进制
- ZIP 格式识别正确；解压后 3 个文件 SHA-256 与源文件完全一致
- 7Z：识别为 `7-Zip`，列出 3 个条目，解压 2 个文件成功
- RAR：识别为 `RAR`，列出 6 个条目，解压成功（1 个符号链接按安全策略跳过）
- RAR5：多个文件列表/解压成功，中文文件名正常
- 非压缩包：被拒绝并返回可读错误码（42 = 格式无法识别）
- Zip Slip：`../../evil.txt` 被拒绝，同包内 `safe.txt` 正常解压
- GUI 端到端：打开 `sample.7z` → 显示 `格式：7-Zip`、3 个条目 → 默认目标目录 →
  “解压到此处” → 进度 100% → “解压成功”弹窗 → 目标目录文件与预期一致

## 已知限制

- **RAR**：libarchive 3.8.2 支持常见 RAR/RAR5 读取；加密 RAR、分卷 RAR、使用了不支持压缩算法的
  RAR 会失败并给出明确提示。0.0.1 不支持创建 RAR（产品范围仅创建 ZIP）。
- **7Z**：支持读取常见 7z；加密 7z 和 libarchive 未编译编码器的 7z 会明确报错，不提供密码输入。
- **压缩格式**：仅创建 ZIP；RAR/7Z 的“解压、列表、进度、错误提示”等读取侧功能一致。
- **文件名编码**：非 UTF-8 的旧式 ZIP（如未设置 UTF-8 标志的 GBK 名称）显示效果取决于 libarchive
  的系统编码转换；StarZip 自身创建的 ZIP 使用 UTF-8 名称。
- **符号链接**：出于安全考虑不创建符号链接/硬链接，跳过并计数提示。
- **路径安全**：跳过绝对路径与 `..` 越界条目，避免 Zip Slip。
- 不实现密码、分卷、修复、右键菜单、CLI、主题切换。
- 目录扫描发生在后台线程；极大目录在扫描阶段只显示“正在扫描”，写出阶段才显示百分比。

## Git 备份

本目录已初始化 Git 仓库，`v0.0.1` 标签指向包含全部源码、测试样例与可运行成品的提交。
