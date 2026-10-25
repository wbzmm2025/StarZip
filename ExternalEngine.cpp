#include "ExternalEngine.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

namespace {

struct RunResult
{
    bool started = false;
    bool canceled = false;
    bool timedOut = false;
    int exitCode = -1;
    QByteArray stdOut;
    QByteArray stdErr;
};

void appendProgressChunk(const QByteArray &chunk, QByteArray *tail, int *lastPercent,
                         const std::atomic_bool *cancel, RunResult *result,
                         const ExternalEngine::ProgressCallback &progress)
{
    *tail += chunk;
    static const QRegularExpression re(QStringLiteral("(\\d{1,3})%"));
    const QString text = QString::fromLatin1(*tail);
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const int percent = qBound(0, match.captured(1).toInt(), 100);
        if (percent != *lastPercent) {
            *lastPercent = percent;
            if (progress.report)
                progress.report(percent, QStringLiteral("正在使用 7-Zip 引擎处理…"));
        }
    }
    if (cancel && cancel->load()) {
        result->canceled = true;
        return;
    }
    if (tail->size() > 64)
        *tail = tail->right(64);
}

RunResult runEngine(const QStringList &arguments, const QString &workingDirectory,
                    const std::atomic_bool *cancel,
                    const ExternalEngine::ProgressCallback &progress,
                    int timeoutMs = 0)
{
    RunResult result;
    const QString engine = ExternalEngine::executablePath();
    if (engine.isEmpty())
        return result;

    QProcess process;
    process.setProgram(engine);
    process.setArguments(arguments);
    if (!workingDirectory.isEmpty())
        process.setWorkingDirectory(workingDirectory);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(10000))
        return result;
    result.started = true;

    QElapsedTimer timer;
    timer.start();
    QByteArray tail;
    int lastPercent = -1;

    while (!process.waitForFinished(120)) {
        const QByteArray chunk = process.readAllStandardOutput();
        if (!chunk.isEmpty()) {
            result.stdOut += chunk;
            appendProgressChunk(chunk, &tail, &lastPercent, cancel, &result, progress);
        }
        const QByteArray errChunk = process.readAllStandardError();
        if (!errChunk.isEmpty())
            result.stdErr += errChunk;

        if (result.canceled || (cancel && cancel->load())) {
            result.canceled = true;
            process.kill();
            process.waitForFinished(3000);
            break;
        }
        if (timeoutMs > 0 && timer.elapsed() > timeoutMs) {
            result.timedOut = true;
            process.kill();
            process.waitForFinished(3000);
            break;
        }
    }

    result.stdOut += process.readAllStandardOutput();
    result.stdErr += process.readAllStandardError();
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(3000);
    }
    result.exitCode = process.exitCode();
    return result;
}

QString engineErrorText(const RunResult &result, const QString &fallback)
{
    QString detail;
    if (!result.stdErr.trimmed().isEmpty())
        detail = QString::fromUtf8(result.stdErr).trimmed();
    else if (!result.stdOut.trimmed().isEmpty())
        detail = QString::fromUtf8(result.stdOut).trimmed();
    if (detail.size() > 1200)
        detail = detail.right(1200);
    if (detail.isEmpty())
        detail = QStringLiteral("7-Zip 引擎退出码：%1").arg(result.exitCode);
    return fallback + QStringLiteral("\n\n") + detail;
}

bool processSucceeded(const RunResult &result)
{
    // 0 = 成功；1 = 有警告但数据已写出；7 = 命令行错误，不能视为成功。
    return result.started && !result.canceled && (result.exitCode == 0 || result.exitCode == 1);
}

QString decodePath(const QString &raw)
{
    QString path = raw;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.startsWith(QStringLiteral("./")))
        path.remove(0, 2);
    return path;
}

QDateTime parseEngineTime(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QDateTime();
    QDateTime dt = QDateTime::fromString(trimmed.left(19),
                                         QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    return dt;
}

bool readEngineListing(const RunResult &result, QVector<ArchiveEntry> *entries,
                       QString *formatName, qint64 *totalBytes, int *entryCount,
                       bool *truncated, QString *error)
{
    const QString text = QString::fromUtf8(result.stdOut);
    const QStringList lines = text.split(QLatin1Char('\n'));
    bool inEntries = false;
    ArchiveEntry current;
    bool hasCurrent = false;
    int count = 0;
    qint64 total = 0;
    QString format;
    static const int limit = 20000;

    auto flush = [&]() {
        if (!hasCurrent)
            return;
        hasCurrent = false;
        const QString path = decodePath(current.path);
        if (path.isEmpty() || path == QStringLiteral("."))
            return;
        current.path = path;
        ++count;
        if (!current.isDir)
            total += current.size;
        if (entries->size() < limit)
            entries->append(current);
        else
            *truncated = true;
    };

    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (!inEntries) {
            if (line == QStringLiteral("----------")) {
                inEntries = true;
                continue;
            }
            if (line.startsWith(QStringLiteral("Type = ")))
                format = line.mid(7).trimmed();
            continue;
        }

        if (line.isEmpty()) {
            flush();
            continue;
        }
        if (line == QStringLiteral("----------")) {
            flush();
            continue;
        }
        if (line.startsWith(QStringLiteral("Path = "))) {
            flush();
            current = ArchiveEntry();
            current.path = line.mid(7);
            hasCurrent = true;
        } else if (line == QStringLiteral("Folder = +")) {
            current.isDir = true;
        } else if (line.startsWith(QStringLiteral("Size = "))) {
            const QString value = line.mid(7).trimmed();
            if (!value.isEmpty())
                current.size = value.toLongLong();
        } else if (line.startsWith(QStringLiteral("Modified = "))) {
            current.modified = parseEngineTime(line.mid(11));
        } else if (line == QStringLiteral("Encrypted = +")) {
            current.encrypted = true;
        }
    }
    flush();

    if (!processSucceeded(result)) {
        *error = engineErrorText(result, QStringLiteral("7-Zip 引擎无法读取该压缩包。"));
        return false;
    }
    if (count == 0) {
        *error = QStringLiteral("7-Zip 引擎没有从该压缩包中读到任何条目。");
        return false;
    }

    if (format.compare(QStringLiteral("Udf"), Qt::CaseInsensitive) == 0)
        format = QStringLiteral("UDF 光盘映像");
    if (format.isEmpty())
        format = QStringLiteral("光盘映像");

    if (formatName)
        *formatName = format;
    if (totalBytes)
        *totalBytes = total;
    if (entryCount)
        *entryCount = count;
    return true;
}

} // namespace

namespace ExternalEngine {

QString executablePath()
{
    static const QString cached = []() -> QString {
        const QByteArray override = qgetenv("STARZIP_7Z_PATH");
        if (!override.isEmpty()) {
            const QString candidate = QString::fromLocal8Bit(override);
            if (QFileInfo(candidate).isFile())
                return QDir::toNativeSeparators(QFileInfo(candidate).absoluteFilePath());
        }

        const QString appDir = QCoreApplication::applicationDirPath();
        const QStringList candidates = {
            QDir(appDir).filePath(QStringLiteral("7z.exe")),
            QDir(appDir).filePath(QStringLiteral("../third_party/7zip/7z.exe")),
            QStringLiteral("C:/Program Files/7-Zip/7z.exe"),
            QStringLiteral("C:/Program Files (x86)/7-Zip/7z.exe"),
        };
        for (const QString &candidate : candidates) {
            if (QFileInfo(candidate).isFile())
                return QDir::toNativeSeparators(QFileInfo(candidate).absoluteFilePath());
        }
        const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("7z"));
        return fromPath.isEmpty() ? QString() : QDir::toNativeSeparators(fromPath);
    }();
    return cached;
}

bool available()
{
    return !executablePath().isEmpty();
}

QString version()
{
    if (!available())
        return QString();
    const RunResult result = runEngine({QStringLiteral("-version")}, QString(), nullptr,
                                       ProgressCallback(), 10000);
    const QString text = QString::fromUtf8(result.stdOut).trimmed();
    return text.section(QLatin1Char('\n'), 0, 0);
}

bool isUdfIso(const QString &archivePath)
{
    const QFileInfo info(archivePath);
    if (info.suffix().compare(QStringLiteral("iso"), Qt::CaseInsensitive) != 0)
        return false;
    QFile file(archivePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray header = file.read(2 * 1024 * 1024);
    return header.contains("NSR02") || header.contains("NSR03");
}

bool list(const QString &archivePath, const QString &password, QVector<ArchiveEntry> *entries,
          QString *formatName, qint64 *totalBytes, int *entryCount, bool *truncated,
          QString *error, const std::atomic_bool *cancel, bool *canceled)
{
    *truncated = false;
    if (!available()) {
        *error = QStringLiteral("该 ISO 使用 UDF 文件系统，libarchive 无法列出其中的文件。\n"
                                "未找到 7-Zip 引擎（7z.exe），请重新安装完整的 StarZip。");
        return false;
    }
    QStringList args = {QStringLiteral("l"), QStringLiteral("-slt"),
                        QStringLiteral("-sccUTF-8"), QStringLiteral("-bd")};
    if (!password.isEmpty())
        args.append(QStringLiteral("-p") + password);
    args.append(archivePath);
    const RunResult result = runEngine(args, QString(), cancel, ProgressCallback(), 0);
    if (result.canceled) {
        *canceled = true;
        *error = QStringLiteral("打开压缩包操作已取消。");
        return false;
    }
    return readEngineListing(result, entries, formatName, totalBytes, entryCount, truncated,
                             error);
}

bool extract(const QString &archivePath, const QString &destination,
             const QStringList &onlyPaths, const QString &password, QString *error,
             const ProgressCallback &progress, const std::atomic_bool *cancel,
             bool *canceled)
{
    if (!available()) {
        *error = QStringLiteral("该 ISO 使用 UDF 文件系统，libarchive 无法解压。\n"
                                "未找到 7-Zip 引擎（7z.exe），请重新安装完整的 StarZip。");
        return false;
    }
    if (!QDir().mkpath(destination)) {
        *error = QStringLiteral("无法创建解压目标目录：%1")
                     .arg(QDir::toNativeSeparators(destination));
        return false;
    }

    QStringList args = {QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-sccUTF-8"),
                        QStringLiteral("-bsp1"), archivePath,
                        QStringLiteral("-o") + destination};
    if (!password.isEmpty())
        args.append(QStringLiteral("-p") + password);
    for (const QString &path : onlyPaths) {
        if (!path.trimmed().isEmpty())
            args.append(path);
    }

    const RunResult result = runEngine(args, QString(), cancel, progress, 0);
    if (result.canceled) {
        *canceled = true;
        *error = QStringLiteral("解压操作已取消。");
        return false;
    }
    if (!processSucceeded(result)) {
        *error = engineErrorText(result, QStringLiteral("7-Zip 引擎解压失败。"));
        return false;
    }
    return true;
}

bool test(const QString &archivePath, const QString &password, QString *error,
          const ProgressCallback &progress, const std::atomic_bool *cancel, bool *canceled)
{
    if (!available()) {
        *error = QStringLiteral("该 ISO 使用 UDF 文件系统，测试需要 7-Zip 引擎（7z.exe）。");
        return false;
    }
    QStringList args = {QStringLiteral("t"), QStringLiteral("-sccUTF-8"),
                        QStringLiteral("-bd")};
    if (!password.isEmpty())
        args.append(QStringLiteral("-p") + password);
    args.append(archivePath);
    const RunResult result = runEngine(args, QString(), cancel, progress, 0);
    if (result.canceled) {
        *canceled = true;
        *error = QStringLiteral("测试操作已取消。");
        return false;
    }
    if (!processSucceeded(result)) {
        *error = engineErrorText(result, QStringLiteral("7-Zip 引擎测试失败。"));
        return false;
    }
    return true;
}

bool compress7z(const QStringList &sources, const QString &outputPath, int level,
                const QString &password, QString *error, const ProgressCallback &progress,
                const std::atomic_bool *cancel, bool *canceled)
{
    if (!available()) {
        *error = QStringLiteral("创建带密码的 7Z 需要 7-Zip 引擎（7z.exe）。\n"
                                "请重新安装完整的 StarZip，或改用 ZIP 格式。");
        return false;
    }
    if (sources.isEmpty()) {
        *error = QStringLiteral("没有选择要压缩的文件或文件夹。");
        return false;
    }

    // 以所有源项目的公共父目录为工作目录，避免把盘符或临时路径写进压缩包。
    QString common = QFileInfo(sources.first()).isDir()
                         ? QFileInfo(sources.first()).absoluteFilePath()
                         : QFileInfo(sources.first()).absolutePath();
    for (const QString &source : sources) {
        QString candidate = QFileInfo(source).isDir()
                                ? QFileInfo(source).absoluteFilePath()
                                : QFileInfo(source).absolutePath();
        while (!common.isEmpty() && !candidate.startsWith(common, Qt::CaseInsensitive)) {
            const int slash = common.lastIndexOf(QLatin1Char('/'));
            if (slash <= 0) {
                common.clear();
                break;
            }
            common.truncate(slash);
        }
    }
    if (common.isEmpty())
        common = QFileInfo(sources.first()).absolutePath();

    QStringList relative;
    for (const QString &source : sources) {
        const QString abs = QFileInfo(source).absoluteFilePath();
        QString rel = QDir(common).relativeFilePath(abs);
        if (rel.isEmpty() || rel.startsWith(QStringLiteral("..")))
            rel = abs;
        relative.append(rel);
    }

    if (!QDir().mkpath(QFileInfo(outputPath).absolutePath())) {
        *error = QStringLiteral("无法创建输出目录：%1")
                     .arg(QDir::toNativeSeparators(QFileInfo(outputPath).absolutePath()));
        return false;
    }
    if (QFileInfo::exists(outputPath))
        QFile::remove(outputPath);

    QStringList args = {QStringLiteral("a"), QStringLiteral("-t7z"),
                        QStringLiteral("-mx=%1").arg(qBound(0, level, 9)),
                        QStringLiteral("-sccUTF-8"), QStringLiteral("-bsp1"),
                        QStringLiteral("-y"),
                        QStringLiteral("-p") + password,
                        outputPath};
    args += relative;

    const RunResult result = runEngine(args, common, cancel, progress, 0);
    if (result.canceled) {
        *canceled = true;
        *error = QStringLiteral("压缩操作已取消。");
        QFile::remove(outputPath);
        return false;
    }
    if (!processSucceeded(result) || !QFileInfo::exists(outputPath)) {
        *error = engineErrorText(result, QStringLiteral("7-Zip 引擎创建压缩包失败。"));
        QFile::remove(outputPath);
        return false;
    }
    return true;
}

} // namespace ExternalEngine
