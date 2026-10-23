// StarZip 核心逻辑自检程序。
// 直接驱动 ArchiveWorker，不依赖 GUI，用于自动化验证 ZIP 压缩/列表/解压
// 以及外部传入的 7Z/RAR 等压缩包读取路径。
#include "ArchiveWorker.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTextStream>

#include <cstdio>

namespace {

bool g_failed = false;

void check(bool ok, const QString &what)
{
    QTextStream(stdout) << (ok ? "[PASS] " : "[FAIL] ") << what << "\n";
    if (!ok)
        g_failed = true;
}

QString sha256(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f))
        return QString();
    return QString::fromLatin1(hash.result().toHex());
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(data) == data.size();
}

struct Result
{
    bool success = false;
    bool canceled = false;
    QString message;
    int errorCode = 0;
};

Result runList(ArchiveWorker &worker, const QString &archive, QString *format = nullptr,
               int *count = nullptr)
{
    Result r;
    QObject::connect(&worker, &ArchiveWorker::listed, &worker,
                     [format, count](const QVector<ArchiveEntry> &entries, const QString &fmt,
                                     qint64, int entryCount, bool) {
                         if (format)
                             *format = fmt;
                         if (count)
                             *count = entryCount;
                         Q_UNUSED(entries);
                     });
    QObject::connect(&worker, &ArchiveWorker::finished, &worker,
                     [&r](int, bool success, bool canceled, const QString &message, int code,
                          const QString &) {
                         r.success = success;
                         r.canceled = canceled;
                         r.message = message;
                         r.errorCode = code;
                     });
    worker.listArchive(archive);
    return r;
}

Result runExtract(ArchiveWorker &worker, const QString &archive, const QString &dest)
{
    Result r;
    QObject::connect(&worker, &ArchiveWorker::finished, &worker,
                     [&r](int, bool success, bool canceled, const QString &message, int code,
                          const QString &) {
                         r.success = success;
                         r.canceled = canceled;
                         r.message = message;
                         r.errorCode = code;
                     });
    worker.extractArchive(archive, dest);
    return r;
}

Result runCompress(ArchiveWorker &worker, const QStringList &sources, const QString &zip)
{
    Result r;
    QObject::connect(&worker, &ArchiveWorker::finished, &worker,
                     [&r](int, bool success, bool canceled, const QString &message, int code,
                          const QString &) {
                         r.success = success;
                         r.canceled = canceled;
                         r.message = message;
                         r.errorCode = code;
                     });
    worker.compressToZip(sources, zip);
    return r;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    const QString work = QDir::currentPath() + QStringLiteral("/test-work");
    QDir(work).removeRecursively();
    QDir().mkpath(work);

    // ---------- 1. 准备源数据（含中文名、子目录、二进制随机数据） ----------
    const QString srcRoot = work + QStringLiteral("/源目录");
    QByteArray large(3 * 1024 * 1024, Qt::Uninitialized);
    for (int i = 0; i < large.size(); ++i)
        large[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
    check(writeFile(srcRoot + QStringLiteral("/文档/说明.txt"),
                    QStringLiteral("StarZip 自检文本\n").toUtf8()),
          QStringLiteral("创建中文路径文本文件"));
    check(writeFile(srcRoot + QStringLiteral("/二进制数据.bin"), large),
          QStringLiteral("创建 3 MiB 随机二进制文件"));
    const QString extraFile = work + QStringLiteral("/额外文件.txt");
    check(writeFile(extraFile, QByteArray("extra file\n")), QStringLiteral("创建单文件源"));

    const QString zipPath = work + QStringLiteral("/self-test.zip");
    const QString outDir = work + QStringLiteral("/extracted");

    // ---------- 2. 压缩为 ZIP ----------
    ArchiveWorker worker;
    Result r = runCompress(worker, {srcRoot, extraFile}, zipPath);
    check(r.success, QStringLiteral("ZIP 压缩：%1").arg(r.message));
    check(QFileInfo(zipPath).size() > 1024, QStringLiteral("ZIP 输出文件大小合理"));

    // ---------- 3. 列表 ----------
    QString format;
    int count = 0;
    r = runList(worker, zipPath, &format, &count);
    check(r.success, QStringLiteral("ZIP 列表：%1").arg(r.message));
    check(format.contains(QStringLiteral("ZIP"), Qt::CaseInsensitive),
          QStringLiteral("识别格式为 ZIP（实际：%1）").arg(format));
    check(count >= 4, QStringLiteral("ZIP 条目数 >= 4（实际：%1）").arg(count));

    // ---------- 4. 解压并逐文件校验 ----------
    r = runExtract(worker, zipPath, outDir);
    check(r.success, QStringLiteral("ZIP 解压：%1").arg(r.message));
    const QString extractedRoot = outDir + QStringLiteral("/源目录");
    check(sha256(extractedRoot + QStringLiteral("/二进制数据.bin"))
              == sha256(srcRoot + QStringLiteral("/二进制数据.bin")),
          QStringLiteral("解压后 3 MiB 文件 SHA-256 一致"));
    check(sha256(extractedRoot + QStringLiteral("/文档/说明.txt"))
              == sha256(srcRoot + QStringLiteral("/文档/说明.txt")),
          QStringLiteral("解压后中文文本 SHA-256 一致"));
    check(sha256(outDir + QStringLiteral("/额外文件.txt")) == sha256(extraFile),
          QStringLiteral("解压后单文件 SHA-256 一致"));

    // ---------- 5. 外部压缩包（7Z / RAR / 其他） ----------
    const QStringList archives = app.arguments().mid(1);
    for (int i = 0; i < archives.size(); ++i) {
        const QString archive = QFileInfo(archives.at(i)).absoluteFilePath();
        QString fmt;
        int entries = 0;
        r = runList(worker, archive, &fmt, &entries);
        check(r.success, QStringLiteral("外部包列表 %1（格式 %2，%3 条目）：%4")
                             .arg(QFileInfo(archive).fileName(), fmt)
                             .arg(entries)
                             .arg(r.message));
        const QString dest = work + QStringLiteral("/external-%1").arg(i);
        r = runExtract(worker, archive, dest);
        check(r.success, QStringLiteral("外部包解压 %1：%2")
                             .arg(QFileInfo(archive).fileName(), r.message));
    }

    // ---------- 6. 错误路径：非压缩包 ----------
    const QString bad = work + QStringLiteral("/not-an-archive.txt");
    writeFile(bad, QByteArray("this is not an archive\n"));
    r = runList(worker, bad);
    check(!r.success && !r.message.isEmpty(),
          QStringLiteral("非压缩包被拒绝并给出可读错误（错误码 %1）").arg(r.errorCode));

    // ---------- 7. 路径穿越（Zip Slip）防护 ----------
    const QString slipZip = QCoreApplication::applicationDirPath()
                            + QStringLiteral("/../tests/samples/zip-slip.zip");
    if (QFileInfo::exists(slipZip)) {
        const QString slipDest = work + QStringLiteral("/slip-check");
        r = runExtract(worker, slipZip, slipDest);
        check(r.success, QStringLiteral("包含 ../ 条目的 ZIP 可继续解压其余安全条目"));
        check(QFileInfo::exists(slipDest + QStringLiteral("/safe.txt")),
              QStringLiteral("安全条目 safe.txt 正常解压"));
        check(!QFileInfo::exists(work + QStringLiteral("/evil.txt"))
                  && !QFileInfo::exists(slipDest + QStringLiteral("/../evil.txt")),
              QStringLiteral("越界条目 ../../evil.txt 被拒绝"));
    }

    // ---------- 8. 清理 ----------
    QDir(work).removeRecursively();

    out << (g_failed ? "RESULT: FAILED\n" : "RESULT: ALL PASSED\n");
    out.flush();
    return g_failed ? 1 : 0;
}
