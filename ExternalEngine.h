#pragma once

#include "ArchiveWorker.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

// 可选的 7-Zip 外部引擎。
//
// libarchive 不支持 UDF 光盘映像，也不能在创建 7Z 时写入密码。
// 这两种情况交由随程序分发的 7z.exe 处理；找不到引擎时由调用方给出明确错误。
namespace ExternalEngine {

// 返回可用的 7z.exe 路径；未找到时返回空字符串。
QString executablePath();
bool available();
QString version();

// 判断是否为使用 UDF 文件系统的 ISO（Windows 安装盘几乎都是这种）。
bool isUdfIso(const QString &archivePath);

struct ProgressCallback
{
    std::function<void(int percent, const QString &detail)> report;
};

bool list(const QString &archivePath, const QString &password, QVector<ArchiveEntry> *entries,
          QString *formatName, qint64 *totalBytes, int *entryCount, bool *truncated,
          QString *error, const std::atomic_bool *cancel, bool *canceled);

bool extract(const QString &archivePath, const QString &destination,
             const QStringList &onlyPaths, const QString &password, QString *error,
             const ProgressCallback &progress, const std::atomic_bool *cancel,
             bool *canceled);

bool test(const QString &archivePath, const QString &password, QString *error,
          const ProgressCallback &progress, const std::atomic_bool *cancel, bool *canceled);

// 使用 7-Zip 创建带密码的 7Z（libarchive 的 7Z 写入器不支持加密）。
bool compress7z(const QStringList &sources, const QString &outputPath, int level,
                const QString &password, QString *error, const ProgressCallback &progress,
                const std::atomic_bool *cancel, bool *canceled);

} // namespace ExternalEngine
