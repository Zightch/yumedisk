#include <QApplication>
#include <QString>

#include "backendHost/BackendHost/BackendHost.h"
#include "Widget/Widget.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setApplicationName("client");
    QApplication::setApplicationDisplayName("Client");

    BackendHost backendHost;
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&backendHost]() {
        QString errorText;

        (void)backendHost.shutdown(&errorText);
    });
    Widget window(&backendHost);
    window.show();
    return QApplication::exec();
}
