#include "MainWindow.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("StarZip"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.0.3"));
    QCoreApplication::setOrganizationName(QStringLiteral("StarZip"));

    MainWindow window;
    window.show();

    // 命令行契约（资源管理器右键菜单使用）：
    //   StarZip.exe "archive.zip"
    //   StarZip.exe --extract-here "archive.zip"
    //   StarZip.exe --extract-to "archive.zip"
    //   StarZip.exe --test "archive.zip"
    //   StarZip.exe --compress "file-or-folder"
    window.applyStartupArguments(QCoreApplication::arguments());

    return app.exec();
}
