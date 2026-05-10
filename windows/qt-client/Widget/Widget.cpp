#include "Widget.h"

#include "backendHost/BackendHost/BackendHost.h"
#include "CreateDiskDialog/CreateDiskDialog.h"
#include "ui_Widget.h"

#include <QHeaderView>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QIcon>
#include <QKeySequence>
#include <QMessageBox>
#include <QMenu>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QTimer>

Widget::Widget(BackendHost* backendHost, QWidget* parent)
    : QWidget(parent), ui(new Ui::Widget), backendHost(backendHost) {
    ui->setupUi(this);
    initializeShellUi();
    initializeInteractions();
    initializeTray();
    refreshView();
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
    setWindowTitle(QStringLiteral("YumeDisk Client"));
    ui->diskTableWidget->setRowCount(0);
    ui->diskTableWidget->horizontalHeader()->setStretchLastSection(true);
    ui->diskTableWidget->horizontalHeader()->setSectionResizeMode(
        0,
        QHeaderView::ResizeToContents);
    ui->diskTableWidget->horizontalHeader()->setSectionResizeMode(
        1,
        QHeaderView::ResizeToContents);
    ui->diskTableWidget->horizontalHeader()->setSectionResizeMode(
        2,
        QHeaderView::ResizeToContents);
    ui->createDiskButton->setEnabled(backendHost != nullptr);
    ui->removeDiskButton->setEnabled(false);
    ui->sessionStateValueLabel->setText(QStringLiteral("未接入宿主后端"));
    ui->logPlainTextEdit->setPlainText(QStringLiteral("[backend] no logs"));
}

void Widget::initializeInteractions() {
    refreshTimer = new QTimer(this);
    refreshTimer->setInterval(1000);

    connect(refreshTimer, &QTimer::timeout, this, [this]() {
        refreshView();
    });
    connect(ui->createDiskButton, &QPushButton::clicked, this, [this]() {
        createDisk();
    });
    connect(ui->removeDiskButton, &QPushButton::clicked, this, [this]() {
        removeDisk();
    });
    connect(ui->quitButton, &QPushButton::clicked, this, [this]() {
        quitClient();
    });
    connect(
        ui->diskTableWidget,
        &QTableWidget::itemSelectionChanged,
        this,
        [this]() {
            updateRemoveButtonState();
        });

    refreshTimer->start();
}

void Widget::showFromTray() {
    refreshView();
    showNormal();
    raise();
    activateWindow();
}

void Widget::initializeTray() {
    auto* quitShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Q")), this);

    trayIcon = new QSystemTrayIcon(this);
    trayMenu = new QMenu(this);
    openAction = trayMenu->addAction("打开主窗口");
    quitAction = trayMenu->addAction("退出");
    openAction->setObjectName(QStringLiteral("yumedisk.tray.open_action"));
    quitAction->setObjectName(QStringLiteral("yumedisk.tray.quit_action"));
    quitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Q")));

    connect(openAction, &QAction::triggered, this, &Widget::showFromTray);
    connect(quitAction, &QAction::triggered, this, &Widget::quitClient);
    connect(quitShortcut, &QShortcut::activated, this, &Widget::quitClient);
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

void Widget::quitClient() {
    if (backendHost != nullptr) {
        QString errorText;

        if (!backendHost->shutdown(&errorText)) {
            QMessageBox::warning(this, QStringLiteral("退出客户端"), errorText);
            return;
        }
    }

    qApp->quit();
}

void Widget::refreshView() {
    if (backendHost == nullptr) {
        return;
    }

    applySnapshot(backendHost->snapshot());
}

void Widget::applySnapshot(const BackendHostSnapshot& snapshot) {
    const bool hadCurrentTarget = hasCurrentTarget();
    const unsigned long selectedTargetId =
        hadCurrentTarget ? currentTargetId() : 0;

    ui->sessionStateValueLabel->setText(snapshot.sessionStateText);
    ui->diskTableWidget->setRowCount(0);
    for (int rowIndex = 0; rowIndex < (int)snapshot.disks.size(); ++rowIndex) {
        const auto& disk = snapshot.disks[(size_t)rowIndex];
        auto* targetIdItem = new QTableWidgetItem(QString::number(disk.targetId));
        auto* lifecycleItem = new QTableWidgetItem(disk.lifecycleText);
        auto* mediaItem = new QTableWidgetItem(disk.mediaText);
        auto* visiblePathItem = new QTableWidgetItem(disk.visiblePathText);

        targetIdItem->setData(Qt::UserRole, QVariant::fromValue(disk.targetId));
        lifecycleItem->setData(Qt::UserRole, QVariant::fromValue(disk.targetId));
        mediaItem->setData(Qt::UserRole, QVariant::fromValue(disk.targetId));
        visiblePathItem->setData(Qt::UserRole, QVariant::fromValue(disk.targetId));

        ui->diskTableWidget->insertRow(rowIndex);
        ui->diskTableWidget->setItem(rowIndex, 0, targetIdItem);
        ui->diskTableWidget->setItem(rowIndex, 1, lifecycleItem);
        ui->diskTableWidget->setItem(rowIndex, 2, mediaItem);
        ui->diskTableWidget->setItem(rowIndex, 3, visiblePathItem);
    }

    if (hadCurrentTarget) {
        for (int rowIndex = 0; rowIndex < ui->diskTableWidget->rowCount(); ++rowIndex) {
            auto* item = ui->diskTableWidget->item(rowIndex, 0);
            if ((item != nullptr) &&
                (item->data(Qt::UserRole).toULongLong() == selectedTargetId)) {
                ui->diskTableWidget->selectRow(rowIndex);
                break;
            }
        }
    }

    const QString nextText = snapshot.logLines.join(QLatin1Char('\n'));
    if (ui->logPlainTextEdit->toPlainText() == nextText) {
        updateRemoveButtonState();
        return;
    }

    ui->logPlainTextEdit->setPlainText(nextText);
    QTextCursor cursor = ui->logPlainTextEdit->textCursor();
    cursor.movePosition(QTextCursor::End);
    ui->logPlainTextEdit->setTextCursor(cursor);
    updateRemoveButtonState();
}

void Widget::updateRemoveButtonState() {
    ui->removeDiskButton->setEnabled(
        (backendHost != nullptr) && hasCurrentTarget());
}

void Widget::createDisk() {
    if (backendHost == nullptr) {
        return;
    }

    CreateDiskDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QString errorText;
    if (!backendHost->createManagedDisk(dialog.createRequest(), &errorText)) {
        QMessageBox::warning(this, QStringLiteral("创建磁盘"), errorText);
        return;
    }

    refreshView();
}

void Widget::removeDisk() {
    if (backendHost == nullptr || !hasCurrentTarget()) {
        return;
    }

    QString errorText;
    if (!backendHost->removeManagedDisk(currentTargetId(), &errorText)) {
        QMessageBox::warning(this, QStringLiteral("删除磁盘"), errorText);
        return;
    }

    refreshView();
}

unsigned long Widget::currentTargetId() const {
    const auto selectedItems = ui->diskTableWidget->selectedItems();

    if (selectedItems.isEmpty()) {
        return 0;
    }

    return (unsigned long)selectedItems.front()->data(Qt::UserRole).toULongLong();
}

bool Widget::hasCurrentTarget() const {
    return !ui->diskTableWidget->selectedItems().isEmpty();
}
