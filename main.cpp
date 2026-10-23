#include "MainWindow.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("StarZip"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.0.1"));
    QCoreApplication::setOrganizationName(QStringLiteral("StarZip"));

    MainWindow window;
    window.show();
    return app.exec();
}
