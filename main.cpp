#include "MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("StarZip"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.0.2"));
    QCoreApplication::setOrganizationName(QStringLiteral("StarZip"));

    MainWindow window;
    window.show();

    // 支持通过命令行/文件关联直接打开压缩包：StarZip.exe archive.zip
    const QStringList args = QCoreApplication::arguments();
    if (args.size() > 1) {
        const QFileInfo candidate(args.at(1));
        if (candidate.exists() && candidate.isFile())
            window.openArchivePath(candidate.absoluteFilePath());
    }

    return app.exec();
}
