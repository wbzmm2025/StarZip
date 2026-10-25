#include "ArchiveWorker.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>

#include <archive.h>
#include <archive_entry.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

namespace {

constexpr int kListEntryLimit = 20000;

// ---------- libarchive RAII ----------
struct ArchiveReadDeleter {
    void operator()(archive *a) const { if (a) archive_read_free(a); }
};
struct ArchiveWriteDeleter {
    void operator()(archive *a) const { if (a) archive_write_free(a); }
};
struct ArchiveEntryDeleter {
    void operator()(archive_entry *e) const { if (e) archive_entry_free(e); }
};
using ArchiveReadPtr = std::unique_ptr<archive, ArchiveReadDeleter>;
using ArchiveWritePtr = std::unique_ptr<archive, ArchiveWriteDeleter>;
using ArchiveEntryPtr = std::unique_ptr<archive_entry, ArchiveEntryDeleter>;

Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString readableErrorCode(int code)
{
    switch (code) {
    case -1:
        return QStringLiteral("libarchive 未分类错误（通常是格式损坏或读取器不支持）");
    case -2:
        return QStringLiteral("libarchive 调用参数错误");
#ifdef EILSEQ
    case EILSEQ:
        return QStringLiteral("文件格式无法识别、已损坏，或使用了当前 libarchive 不支持的压缩/加密方式");
#endif
#ifdef EACCES
    case EACCES:
        return QStringLiteral("权限不足，无法访问文件或目录");
#endif
#ifdef EPERM
    case EPERM:
        return QStringLiteral("操作不被允许");
#endif
#ifdef ENOENT
    case ENOENT:
        return QStringLiteral("文件或目录不存在");
#endif
#ifdef EEXIST
    case EEXIST:
        return QStringLiteral("目标已存在");
#endif
#ifdef ENOSPC
    case ENOSPC:
        return QStringLiteral("磁盘空间不足");
#endif
#ifdef ENOMEM
    case ENOMEM:
        return QStringLiteral("内存不足");
#endif
#ifdef EINVAL
    case EINVAL:
        return QStringLiteral("参数无效或数据损坏");
#endif
#ifdef EIO
    case EIO:
        return QStringLiteral("读写设备发生 I/O 错误");
#endif
#ifdef EISDIR
    case EISDIR:
        return QStringLiteral("目标是目录，不能按文件写入");
#endif
#ifdef ENOTDIR
    case ENOTDIR:
        return QStringLiteral("路径中包含了非目录项");
#endif
#ifdef ENAMETOOLONG
    case ENAMETOOLONG:
        return QStringLiteral("文件名或路径过长");
#endif
#ifdef ELOOP
    case ELOOP:
        return QStringLiteral("符号链接循环");
#endif
#ifdef EFBIG
    case EFBIG:
        return QStringLiteral("文件大小超出限制");
#endif
    default:
        break;
    }
    if (code > 0) {
        const char *text = std::strerror(code);
        if (text && *text)
            return QString::fromLocal8Bit(text);
    }
    if (code < 0)
        return QStringLiteral("libarchive 内部错误");
    return QStringLiteral("未知错误");
}

QString formatNameFromSuffix(const QString &archivePath)
{
    const QString suffix = QFileInfo(archivePath).suffix().toLower();
    if (suffix == QStringLiteral("zip"))
        return QStringLiteral("ZIP");
    if (suffix == QStringLiteral("7z"))
        return QStringLiteral("7-Zip");
    if (suffix == QStringLiteral("rar"))
        return QStringLiteral("RAR");
    if (suffix == QStringLiteral("tar"))
        return QStringLiteral("TAR");
    if (suffix == QStringLiteral("gz") || suffix == QStringLiteral("tgz"))
        return QStringLiteral("gzip");
    if (suffix == QStringLiteral("bz2") || suffix == QStringLiteral("tbz2"))
        return QStringLiteral("bzip2");
    if (suffix == QStringLiteral("xz") || suffix == QStringLiteral("txz"))
        return QStringLiteral("xz");
    if (suffix == QStringLiteral("cab"))
        return QStringLiteral("CAB");
    if (suffix == QStringLiteral("iso"))
        return QStringLiteral("ISO9660");
    return QString();
}

QString formatLabel(const QString &format)
{
    if (format == QStringLiteral("zip"))
        return QStringLiteral("ZIP");
    if (format == QStringLiteral("7z"))
        return QStringLiteral("7Z");
    if (format == QStringLiteral("tar"))
        return QStringLiteral("TAR");
    if (format == QStringLiteral("tar.gz"))
        return QStringLiteral("TAR.GZ");
    if (format == QStringLiteral("tar.xz"))
        return QStringLiteral("TAR.XZ");
    return format.toUpper();
}

QString archiveErrorMessage(archive *a, const QString &context, int fallbackCode = 0)
{
    int code = a ? archive_errno(a) : 0;
    const char *detail = a ? archive_error_string(a) : nullptr;
    if (code == 0)
        code = fallbackCode;

    QString message = context;
    if (detail && *detail)
        message += QStringLiteral("\nlibarchive 信息：") + QString::fromUtf8(detail);
    message += QStringLiteral("\n错误码：%1（%2）")
                   .arg(code)
                   .arg(readableErrorCode(code));
    return message;
}

bool messageMentionsPassword(archive *a)
{
    const char *raw = a ? archive_error_string(a) : nullptr;
    if (!raw || !*raw)
        return false;
    const QString text = QString::fromUtf8(raw).toLower();
    return text.contains(QStringLiteral("passphrase"))
           || text.contains(QStringLiteral("password"))
           || text.contains(QStringLiteral("encrypted"))
           || text.contains(QStringLiteral("encryption"))
           || text.contains(QStringLiteral("decrypt"));
}

QString addFormatHint(const QString &archivePath, const QString &message)
{
    const QString suffix = QFileInfo(archivePath).suffix().toLower();
    if (suffix == QStringLiteral("rar")) {
        return message
               + QStringLiteral("\n\n提示：StarZip 通过 libarchive 读取 RAR/RAR5，支持范围受 libarchive "
                                "版本限制；加密 RAR 可尝试输入密码。分卷、损坏或使用不支持压缩算法的 "
                                "RAR 无法解压，且本版本不支持创建 RAR。");
    }
    if (suffix == QStringLiteral("7z")) {
        return message
               + QStringLiteral("\n\n提示：加密 7Z 可尝试输入密码；使用 libarchive 未编译编码器的 7Z "
                                "会明确报错。");
    }
    return message;
}

// ---------- QFile + libarchive 回调：支持 Unicode 路径，避免 fopen 的本地编码问题 ----------
struct ReadContext {
    QFile *file = nullptr;
    std::array<char, 64 * 1024> buffer{};
};

int readOpenCallback(archive *, void *)
{
    return ARCHIVE_OK;
}

la_ssize_t readCallback(archive *a, void *clientData, const void **buffer)
{
    auto *ctx = static_cast<ReadContext *>(clientData);
    const qint64 count = ctx->file->read(ctx->buffer.data(), qint64(ctx->buffer.size()));
    if (count < 0) {
        archive_set_error(a, errno, "读取压缩包失败");
        return -1;
    }
    *buffer = ctx->buffer.data();
    return static_cast<la_ssize_t>(count);
}

la_int64_t skipCallback(archive *a, void *clientData, la_int64_t request)
{
    if (request < 0)
        return -1;
    auto *ctx = static_cast<ReadContext *>(clientData);
    const qint64 start = ctx->file->pos();
    if (start < 0 || !ctx->file->seek(start + request)) {
        archive_set_error(a, errno, "跳过压缩包数据失败");
        return -1;
    }
    return ctx->file->pos() - start;
}

la_int64_t seekCallback(archive *a, void *clientData, la_int64_t offset, int whence)
{
    auto *ctx = static_cast<ReadContext *>(clientData);
    qint64 base = 0;
    if (whence == SEEK_CUR)
        base = ctx->file->pos();
    else if (whence == SEEK_END)
        base = ctx->file->size();
    else if (whence != SEEK_SET) {
        archive_set_error(a, EINVAL, "无效的定位参数");
        return -1;
    }
    const qint64 target = base + offset;
    if (target < 0 || !ctx->file->seek(target)) {
        archive_set_error(a, errno, "定位压缩包失败");
        return -1;
    }
    return target;
}

int readCloseCallback(archive *, void *)
{
    return ARCHIVE_OK;
}

bool openReader(archive *a, ReadContext &context, const QString &password,
                QString *error, bool *needsPassword)
{
    *needsPassword = false;
    archive_read_support_filter_all(a);
    // format_all 已包含 ZIP / 7Z / RAR / RAR5 等所有本构建启用的读取器。
    // 不要再次单独注册：重复注册同名读取器会触发 libarchive 内部的清理路径。
    archive_read_support_format_all(a);

    if (!password.isEmpty()) {
        const QByteArray utf8 = password.toUtf8();
        archive_read_add_passphrase(a, utf8.constData());
    }

    archive_read_set_callback_data(a, &context);
    archive_read_set_open_callback(a, readOpenCallback);
    archive_read_set_read_callback(a, readCallback);
    archive_read_set_skip_callback(a, skipCallback);
    archive_read_set_seek_callback(a, seekCallback);
    archive_read_set_close_callback(a, readCloseCallback);

    const int result = archive_read_open1(a);
    if (result != ARCHIVE_OK) {
        *needsPassword = messageMentionsPassword(a);
        *error = archiveErrorMessage(a, QStringLiteral("无法打开或识别压缩包。"));
        return false;
    }
    return true;
}

QString entryPath(archive_entry *entry)
{
    if (const char *utf8 = archive_entry_pathname_utf8(entry))
        return QString::fromUtf8(utf8);
    if (const char *raw = archive_entry_pathname(entry))
        return QString::fromLocal8Bit(raw);
    return QString();
}

QDateTime entryModified(archive_entry *entry)
{
    if (!archive_entry_mtime_is_set(entry))
        return QDateTime();
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(archive_entry_mtime(entry)));
}

// 只允许相对路径；拒绝绝对路径、盘符、..、NTFS 数据流等。
bool makeSafeRelativePath(const QString &raw, QString *safePath)
{
    QString path = raw;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));

    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || QDir::isAbsolutePath(path))
        return false;
    if (path.size() >= 2 && path.at(1) == QLatin1Char(':'))
        return false;

    QStringList safeParts;
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QStringLiteral("."))
            continue;
        if (part == QStringLiteral("..") || part.contains(QLatin1Char(':'))
            || part.contains(QChar::Null)) {
            return false;
        }
        safeParts.append(part);
    }
    if (safeParts.isEmpty())
        return false;
    *safePath = safeParts.join(QLatin1Char('/'));
    return true;
}

QString normalizedArchiveKey(const QString &raw)
{
    QString path = raw;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

bool selectionMatches(const QString &rawPath, const QStringList &onlyPaths)
{
    if (onlyPaths.isEmpty())
        return true;
    const QString path = normalizedArchiveKey(rawPath);
    for (const QString &selectedRaw : onlyPaths) {
        const QString selected = normalizedArchiveKey(selectedRaw);
        if (selected.isEmpty())
            continue;
        if (path.compare(selected, pathCaseSensitivity()) == 0)
            return true;
        if (path.startsWith(selected + QLatin1Char('/'), pathCaseSensitivity()))
            return true;
    }
    return false;
}

bool writeAll(QFileDevice &file, const char *data, qint64 size)
{
    qint64 written = 0;
    while (written < size) {
        const qint64 n = file.write(data + written, size - written);
        if (n <= 0)
            return false;
        written += n;
    }
    return true;
}

// ---------- 压缩包写入回调 ----------
struct WriteContext {
    QFileDevice *device = nullptr;
};

int writeOpenCallback(archive *, void *)
{
    return ARCHIVE_OK;
}

la_ssize_t writeCallback(archive *a, void *clientData, const void *buffer, size_t length)
{
    auto *ctx = static_cast<WriteContext *>(clientData);
    const qint64 n = ctx->device->write(static_cast<const char *>(buffer), qint64(length));
    if (n < 0) {
        archive_set_error(a, errno, "写入压缩包文件失败");
        return -1;
    }
    return static_cast<la_ssize_t>(n);
}

int writeCloseCallback(archive *a, void *clientData)
{
    auto *ctx = static_cast<WriteContext *>(clientData);
    if (!ctx->device->flush()) {
        archive_set_error(a, errno, "刷新压缩包文件失败");
        return ARCHIVE_FATAL;
    }
    return ARCHIVE_OK;
}

struct CompressRecord {
    QString diskPath;
    QString archivePath;
    bool isDir = false;
    qint64 size = 0;
    qint64 mtime = 0;
};

struct CompressScan {
    QVector<CompressRecord> records;
    qint64 totalBytes = 0;
    QString error;
    int errorCode = 0;
    bool canceled = false;
};

bool samePath(const QString &left, const QString &right)
{
    return QFileInfo(left).absoluteFilePath().compare(QFileInfo(right).absoluteFilePath(),
                                                      pathCaseSensitivity())
           == 0;
}

QString uniqueRootName(const QString &base, QSet<QString> *used)
{
    QString candidate = base;
    int suffix = 2;
    while (used->contains(candidate.toCaseFolded()))
        candidate = QStringLiteral("%1_%2").arg(base).arg(suffix++);
    used->insert(candidate.toCaseFolded());
    return candidate;
}

QString rootNameFor(const QFileInfo &info)
{
    QString name = info.fileName();
    if (name.isEmpty()) {
        const QString absolute = QDir::fromNativeSeparators(info.absoluteFilePath());
        const QStringList parts = absolute.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        name = parts.isEmpty() ? QStringLiteral("root") : parts.last();
    }
    if (name.isEmpty())
        name = QStringLiteral("root");
    return name;
}

CompressScan scanSources(const QStringList &sources, const QString &outputPath,
                         const std::atomic_bool *cancelRequested)
{
    CompressScan scan;
    QSet<QString> usedRoots;
    QSet<QString> seenSources;
    const QString outputAbsolute = QFileInfo(outputPath).absoluteFilePath();

    for (const QString &source : sources) {
        if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) {
            scan.canceled = true;
            return scan;
        }
        const QFileInfo info(source);
        const QString absoluteSource = info.absoluteFilePath();
        const QString sourceKey = absoluteSource.toCaseFolded();
        if (seenSources.contains(sourceKey))
            continue;
        seenSources.insert(sourceKey);

        if (!info.exists()) {
            scan.error = QStringLiteral("源文件或文件夹不存在：%1").arg(QDir::toNativeSeparators(source));
            return scan;
        }
        if (info.isSymLink()) {
            scan.error = QStringLiteral("不支持将符号链接作为压缩根目录：%1")
                             .arg(QDir::toNativeSeparators(source));
            return scan;
        }

        const QString rootName = uniqueRootName(rootNameFor(info), &usedRoots);

        if (info.isFile()) {
            if (samePath(absoluteSource, outputAbsolute)) {
                scan.error = QStringLiteral("输出压缩包不能与待压缩文件相同：%1")
                                 .arg(QDir::toNativeSeparators(outputPath));
                return scan;
            }
            CompressRecord record;
            record.diskPath = absoluteSource;
            record.archivePath = rootName;
            record.size = info.size();
            record.mtime = info.lastModified().toSecsSinceEpoch();
            scan.totalBytes += record.size;
            scan.records.append(record);
            continue;
        }

        if (!info.isDir()) {
            scan.error = QStringLiteral("不支持的源类型：%1")
                             .arg(QDir::toNativeSeparators(source));
            return scan;
        }

        CompressRecord rootRecord;
        rootRecord.diskPath = absoluteSource;
        rootRecord.archivePath = rootName;
        rootRecord.isDir = true;
        rootRecord.mtime = info.lastModified().toSecsSinceEpoch();
        scan.records.append(rootRecord);

        const QDir::Filters filters = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                                      | QDir::NoSymLinks | QDir::Hidden;
        QDirIterator it(absoluteSource, filters, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) {
                scan.canceled = true;
                return scan;
            }
            const QString filePath = it.next();
            const QFileInfo fileInfo = it.fileInfo();

            if (fileInfo.isSymLink())
                continue;
            if (!fileInfo.isDir() && samePath(filePath, outputAbsolute))
                continue;

            QString relative = QDir::fromNativeSeparators(
                QDir(absoluteSource).relativeFilePath(filePath));
            if (relative.isEmpty() || relative == QStringLiteral("."))
                continue;
            if (relative.startsWith(QStringLiteral("../")))
                continue;

            CompressRecord record;
            record.diskPath = filePath;
            record.archivePath = rootName + QLatin1Char('/') + relative;
            record.isDir = fileInfo.isDir();
            record.size = record.isDir ? 0 : fileInfo.size();
            record.mtime = fileInfo.lastModified().toSecsSinceEpoch();

            if (!record.isDir)
                scan.totalBytes += record.size;
            scan.records.append(record);
        }
    }

    if (scan.records.isEmpty()) {
        scan.error = QStringLiteral("没有可压缩的文件或文件夹。");
    }
    return scan;
}

} // namespace

ArchiveWorker::ArchiveWorker(QObject *parent)
    : QObject(parent)
{
}

void ArchiveWorker::requestCancel()
{
    m_cancelRequested.store(true, std::memory_order_relaxed);
}

bool ArchiveWorker::isCancelRequested() const
{
    return m_cancelRequested.load(std::memory_order_relaxed);
}

void ArchiveWorker::resetCancel()
{
    m_cancelRequested.store(false, std::memory_order_relaxed);
}

void ArchiveWorker::listArchive(const QString &archivePath, const QString &password)
{
    resetCancel();

    QFile file(archivePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit finished(List, false, false,
                      QStringLiteral("无法读取压缩包：%1\n%2")
                          .arg(QDir::toNativeSeparators(archivePath), file.errorString()),
                      0, archivePath);
        return;
    }

    ArchiveReadPtr a(archive_read_new());
    if (!a) {
        emit finished(List, false, false, QStringLiteral("无法创建 libarchive 读取器（内存不足）。"),
                      ENOMEM, archivePath);
        return;
    }

    ReadContext readContext;
    readContext.file = &file;
    QString openError;
    bool needsPassword = false;
    if (!openReader(a.get(), readContext, password, &openError, &needsPassword)) {
        if (needsPassword)
            emit passwordRequired(List, archivePath);
        emit finished(List, false, false, addFormatHint(archivePath, openError),
                      archive_errno(a.get()), archivePath);
        return;
    }

    QVector<ArchiveEntry> entries;
    entries.reserve(64);
    qint64 totalBytes = 0;
    int entryCount = 0;
    bool truncated = false;
    archive_entry *entry = nullptr;
    int result = ARCHIVE_OK;

    while ((result = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK
           || result == ARCHIVE_WARN) {
        if (isCancelRequested()) {
            emit finished(List, false, true, QStringLiteral("打开压缩包操作已取消。"), 0,
                          archivePath);
            return;
        }

        ArchiveEntry item;
        item.path = entryPath(entry);
        const bool pathEndsWithSlash = item.path.endsWith(QLatin1Char('/'))
                                       || item.path.endsWith(QLatin1Char('\\'));
        item.isDir = archive_entry_filetype(entry) == AE_IFDIR || pathEndsWithSlash;
        item.encrypted = archive_entry_is_encrypted(entry) != 0;
        if (!item.isDir && archive_entry_size_is_set(entry))
            item.size = archive_entry_size(entry);
        item.modified = entryModified(entry);

        if (!item.isDir && item.size > 0)
            totalBytes += item.size;

        if (entries.size() < kListEntryLimit) {
            entries.append(item);
        } else {
            truncated = true;
            ++entryCount;
            break;
        }
        ++entryCount;
    }

    if (!truncated && result != ARCHIVE_EOF) {
        if (messageMentionsPassword(a.get()))
            emit passwordRequired(List, archivePath);
        emit finished(List, false, false,
                      addFormatHint(archivePath,
                                    archiveErrorMessage(a.get(),
                                                        QStringLiteral("读取压缩包条目失败。"))),
                      archive_errno(a.get()), archivePath);
        return;
    }

    // 格式名在读取第一个条目头之后才由 libarchive 填入，因此放到循环后获取。
    const char *formatUtf8 = archive_format_name(a.get());
    QString formatName = (formatUtf8 && *formatUtf8) ? QString::fromUtf8(formatUtf8)
                                                     : QString();
    if (formatName.isEmpty())
        formatName = formatNameFromSuffix(archivePath);
    if (formatName.isEmpty())
        formatName = QStringLiteral("未知格式");

    emit listed(entries, formatName, totalBytes, entryCount, truncated);
    emit finished(List, true, false,
                  QStringLiteral("已识别 %1 个条目，格式：%2%3")
                      .arg(entryCount)
                      .arg(formatName)
                      .arg(truncated ? QStringLiteral("（列表仅显示前 %1 项）")
                                            .arg(kListEntryLimit)
                                     : QString()),
                  0, archivePath);
}

void ArchiveWorker::extractArchive(const QString &archivePath, const QString &destinationDir,
                                   const QString &password, const QStringList &onlyPaths)
{
    resetCancel();

    const QString destAbsolute = QDir(destinationDir).absolutePath();
    QFile archiveFile(archivePath);
    if (!archiveFile.open(QIODevice::ReadOnly)) {
        emit finished(Extract, false, false,
                      QStringLiteral("无法读取压缩包：%1\n%2")
                          .arg(QDir::toNativeSeparators(archivePath),
                               archiveFile.errorString()),
                      0, destAbsolute);
        return;
    }

    if (!QDir().mkpath(destAbsolute)) {
        emit finished(Extract, false, false,
                      QStringLiteral("无法创建解压目标目录：%1")
                          .arg(QDir::toNativeSeparators(destAbsolute)),
                      0, destAbsolute);
        return;
    }
    const QFileInfo destInfo(destAbsolute);
    if (!destInfo.isDir()) {
        emit finished(Extract, false, false,
                      QStringLiteral("解压目标不是目录：%1")
                          .arg(QDir::toNativeSeparators(destAbsolute)),
                      0, destAbsolute);
        return;
    }

    ArchiveReadPtr a(archive_read_new());
    if (!a) {
        emit finished(Extract, false, false, QStringLiteral("无法创建 libarchive 读取器（内存不足）。"),
                      ENOMEM, destAbsolute);
        return;
    }

    ReadContext readContext;
    readContext.file = &archiveFile;
    QString openError;
    bool needsPassword = false;
    if (!openReader(a.get(), readContext, password, &openError, &needsPassword)) {
        if (needsPassword)
            emit passwordRequired(Extract, archivePath);
        emit finished(Extract, false, false, addFormatHint(archivePath, openError),
                      archive_errno(a.get()), destAbsolute);
        return;
    }

    const qint64 archiveSize = archiveFile.size();
    QElapsedTimer progressTimer;
    progressTimer.start();
    qint64 lastProgressMs = -1000;
    int lastProgress = -1;

    auto reportProgress = [&](int percent, const QString &detail, bool force) {
        percent = qBound(0, percent, 100);
        const qint64 now = progressTimer.elapsed();
        if (force || percent != lastProgress || now - lastProgressMs >= 80) {
            lastProgress = percent;
            lastProgressMs = now;
            emit progress(percent, detail);
        }
    };

    int entryCount = 0;
    int fileCount = 0;
    int skippedCount = 0;
    int result = ARCHIVE_OK;
    archive_entry *entry = nullptr;

    reportProgress(0, QStringLiteral("正在解压…"), true);

    while ((result = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK
           || result == ARCHIVE_WARN) {
        if (isCancelRequested()) {
            emit finished(Extract, false, true, QStringLiteral("解压操作已取消。"), 0,
                          destAbsolute);
            return;
        }

        ++entryCount;
        const QString rawPath = entryPath(entry);
        if (!selectionMatches(rawPath, onlyPaths)) {
            archive_read_data_skip(a.get());
            continue;
        }

        QString relativePath;
        if (!makeSafeRelativePath(rawPath, &relativePath)) {
            ++skippedCount;
            archive_read_data_skip(a.get());
            continue;
        }

        if (archive_entry_is_encrypted(entry) && password.isEmpty()) {
            emit passwordRequired(Extract, archivePath);
            emit finished(Extract, false, false,
                          addFormatHint(archivePath,
                                        QStringLiteral("压缩包包含加密条目，请输入密码后重试。\n"
                                                       "条目：%1")
                                            .arg(rawPath)),
                          0, destAbsolute);
            return;
        }

        const bool isDir = archive_entry_filetype(entry) == AE_IFDIR
                           || rawPath.endsWith(QLatin1Char('/'))
                           || rawPath.endsWith(QLatin1Char('\\'));
        const bool isLink = archive_entry_filetype(entry) == AE_IFLNK
                            || archive_entry_hardlink(entry) != nullptr;
        const QString targetPath = QDir(destAbsolute).filePath(relativePath);

        // 防止压缩包内条目覆盖正在读取的压缩包本身。
        if (samePath(targetPath, archivePath)) {
            ++skippedCount;
            archive_read_data_skip(a.get());
            continue;
        }

        if (isDir) {
            if (!QDir().mkpath(targetPath)) {
                emit finished(Extract, false, false,
                              QStringLiteral("无法创建目录：%1")
                                  .arg(QDir::toNativeSeparators(targetPath)),
                              0, destAbsolute);
                return;
            }
            continue;
        }

        if (isLink) {
            // 不跟随/创建符号链接，避免链接目标逃逸到目标目录之外。
            ++skippedCount;
            archive_read_data_skip(a.get());
            continue;
        }

        if (archive_entry_filetype(entry) != AE_IFREG
            && archive_entry_filetype(entry) != 0) {
            ++skippedCount;
            archive_read_data_skip(a.get());
            continue;
        }

        const QFileInfo targetInfo(targetPath);
        if (targetInfo.exists() && targetInfo.isDir()) {
            emit finished(Extract, false, false,
                          QStringLiteral("解压条目与已有目录同名，无法写入文件：%1")
                              .arg(QDir::toNativeSeparators(targetPath)),
                          0, destAbsolute);
            return;
        }

        if (!QDir().mkpath(targetInfo.absolutePath())) {
            emit finished(Extract, false, false,
                          QStringLiteral("无法创建目标子目录：%1")
                              .arg(QDir::toNativeSeparators(targetInfo.absolutePath())),
                          0, destAbsolute);
            return;
        }

        QFile outFile(targetPath);
        if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit finished(Extract, false, false,
                          QStringLiteral("无法写入文件：%1\n%2")
                              .arg(QDir::toNativeSeparators(targetPath),
                                   outFile.errorString()),
                          0, destAbsolute);
            return;
        }

        reportProgress(lastProgress < 0 ? 0 : lastProgress,
                       QStringLiteral("正在解压：%1").arg(relativePath), false);

        bool writeFailed = false;
        bool canceled = false;
        QString writeError;
        const void *block = nullptr;
        size_t blockSize = 0;
        la_int64_t blockOffset = 0;
        int dataResult = ARCHIVE_OK;

        while ((dataResult = archive_read_data_block(a.get(), &block, &blockSize, &blockOffset))
               == ARCHIVE_OK) {
            if (isCancelRequested()) {
                canceled = true;
                break;
            }
            if (blockSize == 0)
                continue;
            if (!outFile.seek(blockOffset) || !writeAll(outFile, static_cast<const char *>(block),
                                                        static_cast<qint64>(blockSize))) {
                writeFailed = true;
                writeError = outFile.errorString();
                break;
            }

            const qint64 consumed = archiveSize > 0
                                        ? qMin<qint64>(99, archiveFile.pos() * 100 / archiveSize)
                                        : 50;
            reportProgress(static_cast<int>(consumed),
                           QStringLiteral("正在解压：%1").arg(relativePath), false);
        }

        if (canceled) {
            outFile.close();
            outFile.remove();
            emit finished(Extract, false, true, QStringLiteral("解压操作已取消。"), 0,
                          destAbsolute);
            return;
        }

        if (writeFailed) {
            outFile.close();
            outFile.remove();
            emit finished(Extract, false, false,
                          QStringLiteral("写入解压文件失败：%1\n%2")
                              .arg(QDir::toNativeSeparators(targetPath), writeError),
                          0, destAbsolute);
            return;
        }

        if (dataResult != ARCHIVE_EOF) {
            outFile.close();
            outFile.remove();
            const bool passwordIssue = messageMentionsPassword(a.get());
            if (passwordIssue)
                emit passwordRequired(Extract, archivePath);
            emit finished(Extract, false, false,
                          addFormatHint(archivePath,
                                        archiveErrorMessage(a.get(),
                                                            passwordIssue
                                                                ? QStringLiteral("密码不正确，无法"
                                                                                 "读取条目：%1")
                                                                      .arg(relativePath)
                                                                : QStringLiteral("读取条目数据失败：%1")
                                                                      .arg(relativePath))),
                          archive_errno(a.get()), destAbsolute);
            return;
        }

        outFile.close();
        ++fileCount;
    }

    if (result != ARCHIVE_EOF) {
        const bool passwordIssue = messageMentionsPassword(a.get());
        if (passwordIssue)
            emit passwordRequired(Extract, archivePath);
        emit finished(Extract, false, false,
                      addFormatHint(archivePath,
                                    archiveErrorMessage(a.get(),
                                                        QStringLiteral("解压过程中读取压缩包失败。"))),
                      archive_errno(a.get()), destAbsolute);
        return;
    }

    emit progress(100, QStringLiteral("解压完成"));
    QString message = QStringLiteral("解压完成。\n条目：%1，文件：%2，跳过：%3\n目标目录：%4")
                          .arg(entryCount)
                          .arg(fileCount)
                          .arg(skippedCount)
                          .arg(QDir::toNativeSeparators(destAbsolute));
    if (onlyPaths.isEmpty())
        message += QStringLiteral("\n范围：全部条目");
    else
        message += QStringLiteral("\n范围：选中的 %1 项").arg(onlyPaths.size());
    if (skippedCount > 0) {
        message += QStringLiteral("\n\n注意：已跳过符号链接、特殊文件或路径不安全的条目。");
    }
    emit finished(Extract, true, false, message, 0, destAbsolute);
}

void ArchiveWorker::testArchive(const QString &archivePath, const QString &password)
{
    resetCancel();

    QFile file(archivePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit finished(Test, false, false,
                      QStringLiteral("无法读取压缩包：%1\n%2")
                          .arg(QDir::toNativeSeparators(archivePath), file.errorString()),
                      0, archivePath);
        return;
    }

    ArchiveReadPtr a(archive_read_new());
    if (!a) {
        emit finished(Test, false, false, QStringLiteral("无法创建 libarchive 读取器（内存不足）。"),
                      ENOMEM, archivePath);
        return;
    }

    ReadContext readContext;
    readContext.file = &file;
    QString openError;
    bool needsPassword = false;
    if (!openReader(a.get(), readContext, password, &openError, &needsPassword)) {
        if (needsPassword)
            emit passwordRequired(Test, archivePath);
        emit finished(Test, false, false, addFormatHint(archivePath, openError),
                      archive_errno(a.get()), archivePath);
        return;
    }

    const qint64 archiveSize = file.size();
    QElapsedTimer timer;
    timer.start();
    int entryCount = 0;
    int fileCount = 0;
    qint64 totalBytes = 0;
    int result = ARCHIVE_OK;
    archive_entry *entry = nullptr;

    emit progress(0, QStringLiteral("正在校验压缩包…"));

    while ((result = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK
           || result == ARCHIVE_WARN) {
        if (isCancelRequested()) {
            emit finished(Test, false, true, QStringLiteral("测试操作已取消。"), 0, archivePath);
            return;
        }
        ++entryCount;

        if (archive_entry_is_encrypted(entry) && password.isEmpty()) {
            emit passwordRequired(Test, archivePath);
            emit finished(Test, false, false,
                          addFormatHint(archivePath,
                                        QStringLiteral("压缩包包含加密条目，请输入密码后重试。\n"
                                                       "条目：%1")
                                            .arg(entryPath(entry))),
                          0, archivePath);
            return;
        }

        if (archive_entry_filetype(entry) == AE_IFDIR)
            continue;

        const void *block = nullptr;
        size_t blockSize = 0;
        la_int64_t blockOffset = 0;
        int dataResult = ARCHIVE_OK;
        while ((dataResult = archive_read_data_block(a.get(), &block, &blockSize, &blockOffset))
               == ARCHIVE_OK) {
            if (isCancelRequested()) {
                emit finished(Test, false, true, QStringLiteral("测试操作已取消。"), 0,
                              archivePath);
                return;
            }
            totalBytes += static_cast<qint64>(blockSize);
        }
        if (dataResult != ARCHIVE_EOF) {
            const bool passwordIssue = messageMentionsPassword(a.get());
            if (passwordIssue)
                emit passwordRequired(Test, archivePath);
            emit finished(Test, false, false,
                          addFormatHint(archivePath,
                                        archiveErrorMessage(a.get(),
                                                            QStringLiteral("测试失败：%1")
                                                                .arg(entryPath(entry)))),
                          archive_errno(a.get()), archivePath);
            return;
        }
        ++fileCount;
        if (timer.elapsed() > 80 || fileCount == 1) {
            timer.restart();
            const int percent = archiveSize > 0
                                    ? static_cast<int>(qMin<qint64>(99,
                                                                    file.pos() * 100 / archiveSize))
                                    : 50;
            emit progress(percent, QStringLiteral("正在校验：%1").arg(entryPath(entry)));
        }
    }

    if (result != ARCHIVE_EOF) {
        const bool passwordIssue = messageMentionsPassword(a.get());
        if (passwordIssue)
            emit passwordRequired(Test, archivePath);
        emit finished(Test, false, false,
                      addFormatHint(archivePath,
                                    archiveErrorMessage(a.get(),
                                                        QStringLiteral("测试过程中读取压缩包失败。"))),
                      archive_errno(a.get()), archivePath);
        return;
    }

    emit progress(100, QStringLiteral("校验完成"));
    emit finished(Test, true, false,
                  QStringLiteral("压缩包测试完成，数据完整。\n条目：%1，文件：%2，解压后大小：%3")
                      .arg(entryCount)
                      .arg(fileCount)
                      .arg(totalBytes),
                  0, archivePath);
}

void ArchiveWorker::compressArchive(const QStringList &sources, const QString &outputPath,
                                    const CompressOptions &options)
{
    resetCancel();

    if (sources.isEmpty()) {
        emit finished(Compress, false, false, QStringLiteral("没有选择要压缩的文件或文件夹。"), 0,
                      outputPath);
        return;
    }

    const QString format = options.format.toLower();
    const QString label = formatLabel(format);
    const bool isZip = format == QStringLiteral("zip");
    const bool is7z = format == QStringLiteral("7z");
    const bool isTar = format == QStringLiteral("tar");
    const bool isTarGz = format == QStringLiteral("tar.gz");
    const bool isTarXz = format == QStringLiteral("tar.xz");

    if (!isZip && !is7z && !isTar && !isTarGz && !isTarXz) {
        emit finished(Compress, false, false,
                      QStringLiteral("不支持的输出格式：%1").arg(options.format), 0, outputPath);
        return;
    }
    if (!options.password.isEmpty() && !isZip) {
        emit finished(Compress, false, false,
                      QStringLiteral("0.0.2 仅为 ZIP 写入启用密码；%1 压缩包不能设置密码。\n"
                                     "请选择 ZIP 格式，或清空密码。")
                          .arg(label),
                      0, outputPath);
        return;
    }
    if (!options.password.isEmpty() && options.password.size() < 1) {
        emit finished(Compress, false, false, QStringLiteral("密码不能为空。"), 0, outputPath);
        return;
    }

    emit progress(0, QStringLiteral("正在扫描待压缩内容…"));
    const CompressScan scan = scanSources(sources, outputPath, &m_cancelRequested);
    if (scan.canceled) {
        emit finished(Compress, false, true, QStringLiteral("压缩操作已取消。"), 0, outputPath);
        return;
    }
    if (!scan.error.isEmpty()) {
        emit finished(Compress, false, false, scan.error, scan.errorCode, outputPath);
        return;
    }
    if (isCancelRequested()) {
        emit finished(Compress, false, true, QStringLiteral("压缩操作已取消。"), 0, outputPath);
        return;
    }

    if (QFileInfo(outputPath).exists() && QFileInfo(outputPath).isDir()) {
        emit finished(Compress, false, false,
                      QStringLiteral("输出路径是文件夹，无法保存压缩包：%1")
                          .arg(QDir::toNativeSeparators(outputPath)),
                      0, outputPath);
        return;
    }
    if (!QDir().mkpath(QFileInfo(outputPath).absolutePath())) {
        emit finished(Compress, false, false,
                      QStringLiteral("无法创建输出目录：%1")
                          .arg(QDir::toNativeSeparators(QFileInfo(outputPath).absolutePath())),
                      0, outputPath);
        return;
    }

    QSaveFile outFile(outputPath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        emit finished(Compress, false, false,
                      QStringLiteral("无法创建压缩包：%1\n%2")
                          .arg(QDir::toNativeSeparators(outputPath), outFile.errorString()),
                      0, outputPath);
        return;
    }

    ArchiveWritePtr a(archive_write_new());
    if (!a) {
        outFile.cancelWriting();
        emit finished(Compress, false, false,
                      QStringLiteral("无法创建 libarchive 写入器（内存不足）。"),
                      ENOMEM, outputPath);
        return;
    }
    // 自定义回调写入时 libarchive 默认会补零到 10 KiB 块；7Z 读取器会把尾部填充
    // 判定为损坏，因此要求最后一块按实际字节数写出。
    archive_write_set_bytes_in_last_block(a.get(), 1);

    auto abortCompression = [&](const QString &message, int errorCode) {
        if (a)
            a.reset();
        outFile.cancelWriting();
        emit finished(Compress, false, false, message, errorCode, outputPath);
    };

    const QByteArray levelText = QByteArray::number(qBound(0, options.level, 9));
    int result = ARCHIVE_OK;
    bool zipCryptoFallback = false;

    if (isZip) {
        result = archive_write_set_format_zip(a.get());
        if (result != ARCHIVE_OK) {
            abortCompression(archiveErrorMessage(a.get(), QStringLiteral("无法初始化 ZIP 写入器。")),
                             archive_errno(a.get()));
            return;
        }
        archive_write_set_format_option(a.get(), "zip", "hdrcharset", "UTF-8");
        archive_write_set_format_option(a.get(), "zip", "compression",
                                        options.level <= 0 ? "store" : "deflate");
        archive_write_set_format_option(a.get(), "zip", "compression-level", levelText.constData());
        archive_write_set_format_option(a.get(), "zip", "zip64", "1");

        if (!options.password.isEmpty()) {
            int enc = archive_write_set_format_option(a.get(), "zip", "encryption", "aes256");
            if (enc != ARCHIVE_OK) {
                enc = archive_write_set_format_option(a.get(), "zip", "encryption", "zipcrypt");
                zipCryptoFallback = true;
            }
            if (enc != ARCHIVE_OK) {
                abortCompression(archiveErrorMessage(a.get(),
                                                     QStringLiteral("当前 libarchive 不支持 ZIP "
                                                                    "加密。")),
                                 archive_errno(a.get()));
                return;
            }
            const QByteArray passwordUtf8 = options.password.toUtf8();
            if (archive_write_set_passphrase(a.get(), passwordUtf8.constData()) != ARCHIVE_OK) {
                abortCompression(archiveErrorMessage(a.get(),
                                                     QStringLiteral("设置 ZIP 密码失败。")),
                                 archive_errno(a.get()));
                return;
            }
        }
    } else if (is7z) {
        result = archive_write_set_format_7zip(a.get());
        if (result != ARCHIVE_OK) {
            abortCompression(archiveErrorMessage(a.get(), QStringLiteral("无法初始化 7Z 写入器。")),
                             archive_errno(a.get()));
            return;
        }
        archive_write_set_format_option(a.get(), "7zip", "compression-level", levelText.constData());
    } else {
        result = archive_write_set_format_pax_restricted(a.get());
        if (result != ARCHIVE_OK) {
            abortCompression(archiveErrorMessage(a.get(), QStringLiteral("无法初始化 TAR 写入器。")),
                             archive_errno(a.get()));
            return;
        }
        if (isTarGz) {
            result = archive_write_add_filter_gzip(a.get());
            if (result != ARCHIVE_OK) {
                abortCompression(archiveErrorMessage(a.get(),
                                                     QStringLiteral("无法初始化 gzip 过滤器。")),
                                 archive_errno(a.get()));
                return;
            }
            archive_write_set_filter_option(a.get(), "gzip", "compression-level",
                                            levelText.constData());
        } else if (isTarXz) {
            result = archive_write_add_filter_xz(a.get());
            if (result != ARCHIVE_OK) {
                abortCompression(archiveErrorMessage(a.get(),
                                                     QStringLiteral("无法初始化 xz 过滤器。")),
                                 archive_errno(a.get()));
                return;
            }
            archive_write_set_filter_option(a.get(), "xz", "compression-level",
                                            levelText.constData());
        }
    }

    WriteContext writeContext;
    writeContext.device = &outFile;
    result = archive_write_open(a.get(), &writeContext, writeOpenCallback, writeCallback,
                                writeCloseCallback);
    if (result != ARCHIVE_OK) {
        abortCompression(archiveErrorMessage(a.get(), QStringLiteral("无法打开输出流。")),
                         archive_errno(a.get()));
        return;
    }

    QElapsedTimer progressTimer;
    progressTimer.start();
    qint64 lastProgressMs = -1000;
    int lastPercent = -1;
    auto reportProgress = [&](int percent, const QString &detail, bool force) {
        percent = qBound(0, percent, 100);
        const qint64 now = progressTimer.elapsed();
        if (force || percent != lastPercent || now - lastProgressMs >= 80) {
            lastPercent = percent;
            lastProgressMs = now;
            emit progress(percent, detail);
        }
    };

    qint64 bytesDone = 0;
    int recordsDone = 0;
    std::array<char, 64 * 1024> buffer{};

    for (const CompressRecord &record : scan.records) {
        if (isCancelRequested()) {
            a.reset();
            outFile.cancelWriting();
            emit finished(Compress, false, true, QStringLiteral("压缩操作已取消。"), 0, outputPath);
            return;
        }

        QString archiveName = record.archivePath;
        if (record.isDir && !archiveName.endsWith(QLatin1Char('/')))
            archiveName += QLatin1Char('/');
        const QByteArray archiveNameUtf8 = archiveName.toUtf8();

        ArchiveEntryPtr entry(archive_entry_new());
        if (!entry) {
            abortCompression(QStringLiteral("无法创建压缩条目（内存不足）。"), ENOMEM);
            return;
        }
        archive_entry_set_pathname_utf8(entry.get(), archiveNameUtf8.constData());
        archive_entry_set_filetype(entry.get(), record.isDir ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(entry.get(), record.isDir ? 0755 : 0644);
        archive_entry_set_size(entry.get(), record.isDir ? 0 : record.size);
        archive_entry_set_mtime(entry.get(), record.mtime, 0);

        result = archive_write_header(a.get(), entry.get());
        if (result < ARCHIVE_WARN) {
            abortCompression(archiveErrorMessage(a.get(),
                                                 QStringLiteral("写入 %1 条目头失败：%2")
                                                     .arg(label, record.archivePath)),
                             archive_errno(a.get()));
            return;
        }

        if (!record.isDir) {
            QFile input(record.diskPath);
            if (!input.open(QIODevice::ReadOnly)) {
                abortCompression(QStringLiteral("无法读取源文件：%1\n%2")
                                     .arg(QDir::toNativeSeparators(record.diskPath),
                                          input.errorString()),
                                 0);
                return;
            }

            qint64 remaining = record.size;
            while (remaining > 0) {
                if (isCancelRequested()) {
                    input.close();
                    a.reset();
                    outFile.cancelWriting();
                    emit finished(Compress, false, true, QStringLiteral("压缩操作已取消。"), 0,
                                  outputPath);
                    return;
                }
                const qint64 wanted = qMin<qint64>(qint64(buffer.size()), remaining);
                const qint64 got = input.read(buffer.data(), wanted);
                if (got < 0) {
                    abortCompression(QStringLiteral("读取源文件失败：%1\n%2")
                                         .arg(QDir::toNativeSeparators(record.diskPath),
                                              input.errorString()),
                                     0);
                    return;
                }
                if (got == 0) {
                    abortCompression(QStringLiteral("源文件在压缩过程中被截短：%1")
                                         .arg(QDir::toNativeSeparators(record.diskPath)),
                                     0);
                    return;
                }
                qint64 dataOffset = 0;
                while (dataOffset < got) {
                    const la_ssize_t written = archive_write_data(
                        a.get(), buffer.data() + dataOffset,
                        static_cast<size_t>(got - dataOffset));
                    if (written < 0) {
                        abortCompression(archiveErrorMessage(a.get(),
                                                             QStringLiteral("写入 %1 数据失败：%2")
                                                                 .arg(label, record.archivePath)),
                                         archive_errno(a.get()));
                        return;
                    }
                    if (written == 0) {
                        abortCompression(QStringLiteral("压缩写入器没有接受数据：%1")
                                             .arg(record.archivePath),
                                         0);
                        return;
                    }
                    dataOffset += written;
                }
                remaining -= got;
                bytesDone += got;
                const int percent = scan.totalBytes > 0
                                        ? static_cast<int>(qMin<qint64>(
                                              99, bytesDone * 100 / scan.totalBytes))
                                        : static_cast<int>(
                                              qMin<qint64>(99, qint64(recordsDone + 1) * 100
                                                                    / scan.records.size()));
                reportProgress(percent,
                               QStringLiteral("正在压缩：%1").arg(record.archivePath), false);
            }
            input.close();
        }

        result = archive_write_finish_entry(a.get());
        if (result < ARCHIVE_WARN) {
            abortCompression(archiveErrorMessage(a.get(),
                                                 QStringLiteral("完成 %1 条目失败：%2")
                                                     .arg(label, record.archivePath)),
                             archive_errno(a.get()));
            return;
        }
        ++recordsDone;
    }

    result = archive_write_close(a.get());
    if (result != ARCHIVE_OK) {
        const QString error = archiveErrorMessage(a.get(),
                                                  QStringLiteral("关闭 %1 压缩包失败。").arg(label));
        const int code = archive_errno(a.get());
        a.reset();
        outFile.cancelWriting();
        emit finished(Compress, false, false, error, code, outputPath);
        return;
    }
    a.reset();

    if (!outFile.commit()) {
        emit finished(Compress, false, false,
                      QStringLiteral("保存压缩包失败：%1\n%2")
                          .arg(QDir::toNativeSeparators(outputPath), outFile.errorString()),
                      0, outputPath);
        return;
    }

    emit progress(100, QStringLiteral("压缩完成"));
    QString message = QStringLiteral("%1 压缩完成。\n条目：%2\n输出文件：%3")
                          .arg(label)
                          .arg(scan.records.size())
                          .arg(QDir::toNativeSeparators(outputPath));
    if (!options.password.isEmpty()) {
        message += QStringLiteral("\n加密：已启用（%1）")
                       .arg(zipCryptoFallback ? QStringLiteral("ZipCrypto（兼容模式）")
                                              : QStringLiteral("AES-256"));
    }
    emit finished(Compress, true, false, message, 0, outputPath);
}
