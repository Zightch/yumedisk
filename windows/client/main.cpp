#include <QApplication>
#include <QString>

#include "backend/Backend.h"
#include "Widget/Widget.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setApplicationName("client");
    QApplication::setApplicationDisplayName("Client");

    Backend backend;
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&backend]() {
        QString errorText;

        (void)backend.shutdown(&errorText);
    });
    Widget window(&backend);
    window.show();
    return QApplication::exec();
}
