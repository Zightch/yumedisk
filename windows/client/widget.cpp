#include "widget.h"

#include "./ui_widget.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QIcon>
#include <QMenu>
#include <QStyle>
#include <QSystemTrayIcon>

Widget::Widget(QWidget* parent) : QWidget(parent), ui(new Ui::Widget) {
    ui->setupUi(this);
    initializeTray();
}

Widget::~Widget() {
    if (trayIcon != nullptr) {
        trayIcon->hide();
    }

    delete ui;
}

void Widget::closeEvent(QCloseEvent* event) {
    hide();
    event->ignore();
}

void Widget::showFromTray() {
    showNormal();
    raise();
    activateWindow();
}

void Widget::initializeTray() {
    trayIcon = new QSystemTrayIcon(this);
    trayMenu = new QMenu(this);
    openAction = trayMenu->addAction("打开主窗口");
    quitAction = trayMenu->addAction("退出");

    connect(openAction, &QAction::triggered, this, &Widget::showFromTray);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
    connect(
        trayIcon,
        &QSystemTrayIcon::activated,
        this,
        [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger ||
                reason == QSystemTrayIcon::DoubleClick) {
                showFromTray();
            }
        });

    trayIcon->setContextMenu(trayMenu);
    trayIcon->setToolTip("Client");
    trayIcon->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    trayIcon->show();
}
