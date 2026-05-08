#include "widget.h"

#include "client_backend.h"
#include "ui_widget.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QIcon>
#include <QMenu>
#include <QStyle>
#include <QSystemTrayIcon>

Widget::Widget(ClientBackend* backend, QWidget* parent)
    : QWidget(parent), ui(new Ui::Widget), backend(backend) {
    ui->setupUi(this);
    initializeShellUi();
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

void Widget::initializeShellUi() {
    setAccessibleName(QStringLiteral("yumedisk.client.main_window"));
    ui->sessionStateValueLabel->setText(
        backend != nullptr ? backend->sessionStateText()
                           : QStringLiteral("未接入宿主后端"));
    ui->diskTableWidget->setRowCount(0);
    ui->createDiskButton->setEnabled(false);
    ui->removeDiskButton->setEnabled(false);

    if (backend != nullptr) {
        ui->logPlainTextEdit->setPlainText(
            backend->initialLogLines().join(QLatin1Char('\n')));
    } else {
        ui->logPlainTextEdit->setPlainText(
            QStringLiteral("[shell] backend placeholder missing"));
    }
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
    openAction->setObjectName(QStringLiteral("yumedisk.tray.open_action"));
    quitAction->setObjectName(QStringLiteral("yumedisk.tray.quit_action"));

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
    trayIcon->setToolTip("YumeDisk Client");
    trayIcon->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    trayIcon->show();
}
