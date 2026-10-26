#include "ShellIntegration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#endif

namespace {

#ifdef Q_OS_WIN

const QStringList kArchiveExtensions = {
    QStringLiteral(".zip"),  QStringLiteral(".7z"),  QStringLiteral(".rar"),
    QStringLiteral(".tar"),  QStringLiteral(".gz"),  QStringLiteral(".tgz"),
    QStringLiteral(".bz2"),  QStringLiteral(".xz"),  QStringLiteral(".lzma"),
    QStringLiteral(".cab"),  QStringLiteral(".iso"),  QStringLiteral(".stz"),
};

QString applicationPath()
{
    return QDir::toNativeSeparators(QFileInfo(QCoreApplication::applicationFilePath())
                                        .absoluteFilePath());
}

QString commandFor(const QString &arguments)
{
    return QStringLiteral("\"%1\" %2").arg(applicationPath(), arguments);
}

QSettings *registry()
{
    static QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"),
                         QSettings::NativeFormat);
    return &reg;
}

void writeVerb(QSettings &reg, const QString &base, const QString &label,
               const QString &arguments)
{
    reg.setValue(base + QStringLiteral("/Default"), label);
    reg.setValue(base + QStringLiteral("/Icon"), applicationPath() + QStringLiteral(",0"));
    reg.setValue(base + QStringLiteral("/command/Default"), commandFor(arguments));
}

void notifyShell()
{
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

#endif

} // namespace

namespace ShellIntegration {

bool supported()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool installed()
{
#ifdef Q_OS_WIN
    QSettings &reg = *registry();
    return reg.contains(QStringLiteral("Applications/StarZip.exe/shell/open/command/Default"));
#else
    return false;
#endif
}

bool install(QString *error)
{
#ifdef Q_OS_WIN
    QSettings &reg = *registry();
    reg.setValue(QStringLiteral("Applications/StarZip.exe/ApplicationName"),
                 QStringLiteral("StarZip"));
    reg.setValue(QStringLiteral("Applications/StarZip.exe/ApplicationVersion"),
                 QCoreApplication::applicationVersion());
    reg.setValue(QStringLiteral("Applications/StarZip.exe/ApplicationCompany"),
                 QStringLiteral("StarZip"));
    reg.setValue(QStringLiteral("Applications/StarZip.exe/shell/open/command/Default"),
                 commandFor(QStringLiteral("\"%1\"")));
    for (const QString &ext : kArchiveExtensions)
        reg.setValue(QStringLiteral("Applications/StarZip.exe/SupportedTypes/") + ext,
                     QString());

    // 所有文件和文件夹：使用 StarZip 压缩。
    writeVerb(reg, QStringLiteral("*/shell/StarZip.Compress"),
              QStringLiteral("使用 StarZip 压缩…"), QStringLiteral("--compress \"%1\""));
    writeVerb(reg, QStringLiteral("Directory/shell/StarZip.Compress"),
              QStringLiteral("使用 StarZip 压缩…"), QStringLiteral("--compress \"%1\""));

    // 压缩包：打开 / 解压到当前文件夹 / 解压到…
    for (const QString &ext : kArchiveExtensions) {
        const QString base = QStringLiteral("SystemFileAssociations/") + ext;
        writeVerb(reg, base + QStringLiteral("/shell/StarZip.Open"),
                  QStringLiteral("使用 StarZip 打开"), QStringLiteral("\"%1\""));
        writeVerb(reg, base + QStringLiteral("/shell/StarZip.ExtractHere"),
                  QStringLiteral("解压到当前文件夹"), QStringLiteral("--extract-here \"%1\""));
        writeVerb(reg, base + QStringLiteral("/shell/StarZip.ExtractTo"),
                  QStringLiteral("解压到…"), QStringLiteral("--extract-to \"%1\""));
    }

    reg.sync();
    if (reg.status() != QSettings::NoError) {
        if (error)
            *error = QStringLiteral("写入注册表失败，请检查当前用户权限。");
        return false;
    }
    notifyShell();
    return true;
#else
    Q_UNUSED(error);
    return false;
#endif
}

bool uninstall(QString *error)
{
#ifdef Q_OS_WIN
    QSettings &reg = *registry();
    reg.remove(QStringLiteral("Applications/StarZip.exe"));
    reg.remove(QStringLiteral("*/shell/StarZip.Compress"));
    reg.remove(QStringLiteral("Directory/shell/StarZip.Compress"));
    for (const QString &ext : kArchiveExtensions) {
        const QString base = QStringLiteral("SystemFileAssociations/") + ext + QStringLiteral("/shell");
        reg.remove(base + QStringLiteral("/StarZip.Open"));
        reg.remove(base + QStringLiteral("/StarZip.ExtractHere"));
        reg.remove(base + QStringLiteral("/StarZip.ExtractTo"));
    }
    reg.sync();
    if (reg.status() != QSettings::NoError) {
        if (error)
            *error = QStringLiteral("删除注册表项失败。");
        return false;
    }
    notifyShell();
    return true;
#else
    Q_UNUSED(error);
    return false;
#endif
}

} // namespace ShellIntegration
