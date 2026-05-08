#include <QApplication>

#include "widget.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    Widget widget;

    widget.show();
    return QApplication::exec();
}
