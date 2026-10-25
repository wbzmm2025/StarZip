// StarZip 核心逻辑自检程序（0.0.2）。
// 直接驱动 ArchiveWorker，不依赖 GUI，用于自动化验证：
//   ZIP / 7Z / TAR / TAR.GZ / TAR.XZ 创建、列表、解压、校验，
//   ZIP 密码（AES-256 / ZipCrypto）、选择性解压、路径穿越防护，
//   以及外部传入的 7Z / RAR / RAR5 等压缩包读取路径。
#include "ArchiveWorker.h"
#include "ExternalEngine.h"
#include "ShellIntegration.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
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
    QString format;
    QString outputPath;
    QVector<ArchiveEntry> entries;
    int errorCode = 0;
    int entryCount = 0;
    qint64 totalBytes = 0;
    int passwordRequests = 0;
};

// 每次调用都创建独立的 worker，避免信号连接在多次测试之间累积、悬空。
Result runList(const QString &archive, const QString &password = QString())
{
    Result r;
    ArchiveWorker worker;
    QObject::connect(&worker, &ArchiveWorker::listed, &worker,
                     [&r](const QVector<ArchiveEntry> &entries, const QString &fmt,
                          qint64 totalBytes, int entryCount, bool) {
                         r.entries = entries;
                         r.format = fmt;
                         r.totalBytes = totalBytes;
                         r.entryCount = entryCount;
                     });
    QObject::connect(&worker, &ArchiveWorker::passwordRequired, &worker,
                     [&r](int, const QString &) { ++r.passwordRequests; });
    QObject::connect(&worker, &ArchiveWorker::finished, &worker,
                     [&r](int, bool success, bool canceled, const QString &message, int code,
                          const QString &output) {
                         r.success = success;
                         r.canceled = canceled;
                         r.message = message;
                         r.errorCode = code;
                         r.outputPath = output;
                     });
    worker.listArchive(archive, password);
    return r;
}

Result runExtract(const QString &archive, const QString &dest,
                  const QString &password = QString(),
                  const QStringList &onlyPaths = QStringList())
{
    Result r;
    ArchiveWorker worker;
    QObject::connect(&worker, &ArchiveWorker::passwordRequired, &worker,
                     [&r](int, const QString &) { ++r.passwordRequests; });
    QObject::connect(&worker, &ArchiveWorker::finished, &worker,
                     [&r](int, bool success, bool canceled, const QString &message, int code,
                          const QString &output) {
                         r.success = success;
                         r.canceled = canceled;
                         r.message = message;
                         r.errorCode = code;
                         r.outputPath = output;
                     });
    worker.extractArchive(archive, dest, password, onlyPaths);
    return r;
}

Result runTest(const QString &archive, const QString &password = QString())
{
    Result r;
    ArchiveWorker worker;
    QObject::connect(&worker, &ArchiveWorker::passwordRequired, &worker,
                     [&r](int, const QString &) { ++r.passwordRequests; });
    QObject::connect(&worker, &ArchiveWorker::finished, &worker,
                     [&r](int, bool success, bool canceled, const QString &message, int code,
                          const QString &output) {
                         r.success = success;
                         r.canceled = canceled;
                         r.message = message;
                         r.errorCode = code;
                         r.outputPath = output;
                     });
    worker.testArchive(archive, password);
    return r;
}

Result runCompress(const QStringList &sources, const QString &output,
                   const CompressOptions &options)
{
    Result r;
    ArchiveWorker worker;
    QObject::connect(&worker, &ArchiveWorker::finished, &worker,
                     [&r](int, bool success, bool canceled, const QString &message, int code,
                          const QString &outputPath) {
                         r.success = success;
                         r.canceled = canceled;
                         r.message = message;
                         r.errorCode = code;
                         r.outputPath = outputPath;
                     });
    worker.compressArchive(sources, output, options);
    return r;
}

bool sameHash(const QString &left, const QString &right)
{
    const QString a = sha256(left);
    const QString b = sha256(right);
    return !a.isEmpty() && a == b;
}

// 对一个压缩包执行：创建 → 列表 → 解压 → 逐文件哈希校验。
void verifyRoundTrip(const QString &label, const QString &format, const QString &suffix,
                     const QStringList &sources, const QString &work,
                     const QString &srcRoot, const QString &extraFile,
                     const QString &password = QString())
{
    const QString output = work + QStringLiteral("/roundtrip-%1%2%3")
                                       .arg(format, password.isEmpty()
                                                         ? QString()
                                                         : QStringLiteral("-protected"),
                                            suffix);
    CompressOptions options;
    options.format = format;
    options.level = 6;
    options.password = password;

    Result r = runCompress(sources, output, options);
    check(r.success, QStringLiteral("%1 压缩：%2").arg(label, r.message));
    check(QFileInfo(output).size() > 0,
          QStringLiteral("%1 输出文件存在（%2 字节）")
              .arg(label)
              .arg(QFileInfo(output).size()));
    if (!r.success)
        return;

    r = runList(output, password);
    check(r.success, QStringLiteral("%1 列表：%2").arg(label, r.message));
    check(r.entryCount >= 4,
          QStringLiteral("%1 条目数 >= 4（实际 %2）").arg(label).arg(r.entryCount));
    check(!r.format.isEmpty(), QStringLiteral("%1 识别到格式：%2").arg(label, r.format));

    if (!password.isEmpty()) {
        bool encryptedFlagSeen = false;
        for (const ArchiveEntry &entry : r.entries)
            encryptedFlagSeen = encryptedFlagSeen || entry.encrypted;
        check(encryptedFlagSeen, QStringLiteral("%1 列表标记了加密条目").arg(label));
    }

    const QString dest = work + QStringLiteral("/out-%1").arg(format);
    r = runExtract(output, dest, password);
    check(r.success, QStringLiteral("%1 解压：%2").arg(label, r.message));
    check(sameHash(dest + QStringLiteral("/源目录/二进制数据.bin"),
                   srcRoot + QStringLiteral("/二进制数据.bin")),
          QStringLiteral("%1 解压后二进制 SHA-256 一致").arg(label));
    check(sameHash(dest + QStringLiteral("/源目录/文档/说明.txt"),
                   srcRoot + QStringLiteral("/文档/说明.txt")),
          QStringLiteral("%1 解压后中文文本 SHA-256 一致").arg(label));
    check(sameHash(dest + QStringLiteral("/额外文件.txt"), extraFile),
          QStringLiteral("%1 解压后单文件 SHA-256 一致").arg(label));

    r = runTest(output, password);
    check(r.success, QStringLiteral("%1 测试/CRC：%2").arg(label, r.message));
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
    QByteArray large(2 * 1024 * 1024, Qt::Uninitialized);
    for (int i = 0; i < large.size(); ++i)
        large[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
    check(writeFile(srcRoot + QStringLiteral("/文档/说明.txt"),
                    QStringLiteral("StarZip 自检文本\n").toUtf8()),
          QStringLiteral("创建中文路径文本文件"));
    check(writeFile(srcRoot + QStringLiteral("/文档/嵌套/深层.txt"),
                    QStringLiteral("nested\n").toUtf8()),
          QStringLiteral("创建多层目录文件"));
    check(writeFile(srcRoot + QStringLiteral("/二进制数据.bin"), large),
          QStringLiteral("创建 2 MiB 随机二进制文件"));
    const QString extraFile = work + QStringLiteral("/额外文件.txt");
    check(writeFile(extraFile, QByteArray("extra file\n")), QStringLiteral("创建单文件源"));

    const QStringList sources = {srcRoot, extraFile};

    // ---------- 2. 五种格式的创建 / 列表 / 解压 / 校验 ----------
    verifyRoundTrip(QStringLiteral("ZIP"), QStringLiteral("zip"), QStringLiteral(".zip"), sources,
                    work, srcRoot, extraFile);
    verifyRoundTrip(QStringLiteral("7Z"), QStringLiteral("7z"), QStringLiteral(".7z"), sources,
                    work, srcRoot, extraFile);
    verifyRoundTrip(QStringLiteral("TAR"), QStringLiteral("tar"), QStringLiteral(".tar"), sources,
                    work, srcRoot, extraFile);
    verifyRoundTrip(QStringLiteral("TAR.GZ"), QStringLiteral("tar.gz"), QStringLiteral(".tar.gz"),
                    sources, work, srcRoot, extraFile);
    verifyRoundTrip(QStringLiteral("TAR.XZ"), QStringLiteral("tar.xz"), QStringLiteral(".tar.xz"),
                    sources, work, srcRoot, extraFile);

    // ---------- 3. ZIP 密码（AES-256） ----------
    const QString password = QStringLiteral("StarZip#2026");
    verifyRoundTrip(QStringLiteral("ZIP+密码"), QStringLiteral("zip"), QStringLiteral(".zip"),
                    sources, work, srcRoot, extraFile, password);

    const QString encryptedZip = work + QStringLiteral("/roundtrip-zip-protected.zip");
    Result r = runList(encryptedZip);
    check(r.success || r.passwordRequests > 0,
          QStringLiteral("无密码列表加密 ZIP 可识别或提示密码：%1").arg(r.message));

    const QString noPwdDest = work + QStringLiteral("/encrypted-no-pwd");
    r = runExtract(encryptedZip, noPwdDest);
    check(!r.success && r.passwordRequests > 0,
          QStringLiteral("无密码解压加密 ZIP 被拒绝并请求密码：%1").arg(r.message));

    const QString wrongPwdDest = work + QStringLiteral("/encrypted-wrong-pwd");
    r = runExtract(encryptedZip, wrongPwdDest, QStringLiteral("wrong-password"));
    check(!r.success, QStringLiteral("错误密码解压加密 ZIP 失败：%1").arg(r.message));

    // ---------- 4. 选择性解压 ----------
    const QString plainZip = work + QStringLiteral("/roundtrip-zip.zip");
    const QString selectiveDest = work + QStringLiteral("/selective");
    r = runExtract(plainZip, selectiveDest, QString(),
                   {QStringLiteral("源目录/文档")});
    check(r.success, QStringLiteral("选择性解压：%1").arg(r.message));
    check(QFileInfo::exists(selectiveDest + QStringLiteral("/源目录/文档/说明.txt")),
          QStringLiteral("选中的 文档/说明.txt 已解压"));
    check(QFileInfo::exists(selectiveDest + QStringLiteral("/源目录/文档/嵌套/深层.txt")),
          QStringLiteral("选中目录下的子文件已解压"));
    check(!QFileInfo::exists(selectiveDest + QStringLiteral("/源目录/二进制数据.bin")),
          QStringLiteral("未选中的 二进制数据.bin 未被解压"));
    check(!QFileInfo::exists(selectiveDest + QStringLiteral("/额外文件.txt")),
          QStringLiteral("未选中的根文件未被解压"));

    // ---------- 5. 外部压缩包（7Z / RAR / 其他） ----------
    const QStringList archives = app.arguments().mid(1);
    for (int i = 0; i < archives.size(); ++i) {
        const QString archive = QFileInfo(archives.at(i)).absoluteFilePath();
        r = runList(archive);
        check(r.success, QStringLiteral("外部包列表 %1（格式 %2，%3 条目）：%4")
                             .arg(QFileInfo(archive).fileName(), r.format)
                             .arg(r.entryCount)
                             .arg(r.message));
        const QString dest = work + QStringLiteral("/external-%1").arg(i);
        r = runExtract(archive, dest);
        check(r.success, QStringLiteral("外部包解压 %1：%2")
                             .arg(QFileInfo(archive).fileName(), r.message));
        r = runTest(archive);
        check(r.success, QStringLiteral("外部包测试 %1：%2")
                             .arg(QFileInfo(archive).fileName(), r.message));
    }

    // ---------- 6. 错误路径：非压缩包 ----------
    const QString bad = work + QStringLiteral("/not-an-archive.txt");
    writeFile(bad, QByteArray("this is not an archive\n"));
    r = runList(bad);
    check(!r.success && !r.message.isEmpty(),
          QStringLiteral("非压缩包被拒绝并给出可读错误（错误码 %1）").arg(r.errorCode));
    r = runTest(bad);
    check(!r.success, QStringLiteral("非压缩包测试被拒绝"));

    // ---------- 7. 不支持的格式 / 非法参数 ----------
    CompressOptions badFormat;
    badFormat.format = QStringLiteral("rar");
    r = runCompress(sources, work + QStringLiteral("/bad.rar"), badFormat);
    check(!r.success && r.message.contains(QStringLiteral("不支持")),
          QStringLiteral("创建 RAR 被明确拒绝：%1").arg(r.message));

    CompressOptions pwd7z;
    pwd7z.format = QStringLiteral("7z");
    pwd7z.password = QStringLiteral("x");
    const QString pwd7zPath = work + QStringLiteral("/pwd.7z");
    r = runCompress(sources, pwd7zPath, pwd7z);
    if (ExternalEngine::available()) {
        check(r.success && QFileInfo::exists(pwd7zPath),
              QStringLiteral("7Z 设置密码由 7-Zip 引擎创建：%1").arg(r.message));

        const QString noPwdDest = work + QStringLiteral("/pwd7z-nopwd");
        r = runExtract(pwd7zPath, noPwdDest);
        check(!r.success || r.passwordRequests > 0,
              QStringLiteral("无密码解压加密 7Z 被拒绝或请求密码"));

        const QString pwdDest = work + QStringLiteral("/pwd7z-ok");
        r = runExtract(pwd7zPath, pwdDest, QStringLiteral("x"));
        check(r.success && QFileInfo::exists(pwdDest + QStringLiteral("/源目录/二进制数据.bin")),
              QStringLiteral("正确密码可解压 7Z：%1").arg(r.message));
        r = runTest(pwd7zPath, QStringLiteral("x"));
        check(r.success, QStringLiteral("加密 7Z 测试通过"));
    } else {
        check(!r.success && r.message.contains(QStringLiteral("7-Zip")),
              QStringLiteral("缺少 7-Zip 引擎时 7Z 密码被明确拒绝：%1").arg(r.message));
    }

    // ---------- 8. 路径穿越（Zip Slip）防护 ----------
    if (qEnvironmentVariableIsSet("STARZIP_TEST_SHELL")) {
        QString shellError;
        const bool ok = ShellIntegration::install(&shellError);
        check(ok, QStringLiteral("资源管理器集成安装成功：%1").arg(shellError));
        check(ShellIntegration::installed(), QStringLiteral("资源管理器集成注册表可检测"));

        const bool doUninstall = qEnvironmentVariableIntValue("STARZIP_TEST_SHELL_UNINSTALL") != 0;
        if (doUninstall) {
            shellError.clear();
            const bool removed = ShellIntegration::uninstall(&shellError);
            check(removed, QStringLiteral("资源管理器集成移除成功：%1").arg(shellError));
            check(!ShellIntegration::installed(),
                  QStringLiteral("资源管理器集成移除后注册表可检测"));
        }
    }

    // 可选：设置 STARZIP_UDF_ISO 指向真实 Windows 安装 ISO，验证 UDF 目录树。
    const QString udfIso = qEnvironmentVariable("STARZIP_UDF_ISO");
    if (!udfIso.isEmpty() && QFileInfo::exists(udfIso) && ExternalEngine::available()) {
        r = runList(udfIso);
        check(r.success && r.entryCount > 20,
              QStringLiteral("UDF ISO 通过 7-Zip 引擎列出真实目录树（%1 个条目）")
                  .arg(r.entryCount));
        check(r.format.contains(QStringLiteral("UDF")),
              QStringLiteral("UDF ISO 识别为 UDF：%1").arg(r.format));
        r = runTest(udfIso);
        check(r.success, QStringLiteral("UDF ISO 测试通过"));
    }

    const QString slipZip = QCoreApplication::applicationDirPath()
                            + QStringLiteral("/../tests/samples/zip-slip.zip");
    if (QFileInfo::exists(slipZip)) {
        const QString slipDest = work + QStringLiteral("/slip-check");
        r = runExtract(slipZip, slipDest);
        check(r.success, QStringLiteral("包含 ../ 条目的 ZIP 可继续解压其余安全条目"));
        check(QFileInfo::exists(slipDest + QStringLiteral("/safe.txt")),
              QStringLiteral("安全条目 safe.txt 正常解压"));
        check(!QFileInfo::exists(work + QStringLiteral("/evil.txt"))
                  && !QFileInfo::exists(slipDest + QStringLiteral("/../evil.txt")),
              QStringLiteral("越界条目 ../../evil.txt 被拒绝"));
    }

    // ---------- 9. 清理 ----------
    if (qEnvironmentVariableIsEmpty("STARZIP_KEEP_TEST_WORK"))
        QDir(work).removeRecursively();

    out << (g_failed ? "RESULT: FAILED\n" : "RESULT: ALL PASSED\n");
    out.flush();
    return g_failed ? 1 : 0;
}
