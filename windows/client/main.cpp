#include <QApplication>

#include "client_backend.h"
#include "widget/widget.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setApplicationName("client");
    QApplication::setApplicationDisplayName("Client");

    ClientBackend backend;
    Widget window(&backend);
    window.show();
    return QApplication::exec();
}
