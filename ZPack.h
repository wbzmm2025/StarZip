#pragma once

#include <QString>

#include <atomic>
#include <functional>

// ZPack：StarZip 自有的分块自适应 + LZ77 + 规范 Huffman + 位级打包压缩格式。
// 文件后缀为 .stz。单文件无损压缩，带每块 CRC32 校验与存储回退。
namespace ZPack {

constexpr int kCodeTableSize = 316;

struct Progress
{
    std::function<void(int percent, const QString &message)> report;
};

struct Info
{
    quint64 originalSize = 0;
    quint32 blockCount = 0;
    quint64 compressedSize = 0;
    int storedBlocks = 0;
    int huffmanBlocks = 0;
    int lzBlocks = 0;
};

// 检查文件头魔数 "STZ1"。
bool isZPackFile(const QString &path);

// 读取文件头与块统计，不解压数据。
bool inspect(const QString &path, Info *info, QString *error);

// 压缩单个文件。成功时 outputPath 指向完整 .stz 文件。
bool compressFile(const QString &sourcePath, const QString &outputPath, QString *error,
                  const std::atomic_bool *cancel = nullptr,
                  const Progress &progress = Progress());

// 解压 .stz 到 outputPath。outputPath 是最终文件路径，不是目录。
bool decompressFile(const QString &archivePath, const QString &outputPath, QString *error,
                    const std::atomic_bool *cancel = nullptr,
                    const Progress &progress = Progress());

// 完整解码并校验 CRC，不保留输出文件。
bool testFile(const QString &archivePath, QString *error,
              const std::atomic_bool *cancel = nullptr,
              const Progress &progress = Progress());

// 去掉 .stz 后的建议输出文件名。
QString suggestedOutputName(const QString &archivePath);

} // namespace ZPack
