#include "widget.h"

#include "backend/backend.h"
#include "createDiskDialog/createDiskDialog.h"
#include "ui_widget.h"

#include <QHeaderView>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QIcon>
#include <QMessageBox>
#include <QMenu>
#include <QPlainTextEdit>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QTimer>

namespace {

QString fromWide(
    const std::wstring& text)
{
    return QString::fromWCharArray(text.c_str(), (int)text.size());
}

QString mediaModeText(
    clientbackend::MediaMode mode)
{
    switch (mode) {
    case clientbackend::MediaMode::dense:
        return QStringLiteral("dense");
    case clientbackend::MediaMode::sparse:
        return QStringLiteral("sparse");
    default:
        return QStringLiteral("auto");
    }
}

QString visiblePathText(
    const clientbackend::ManagedDiskSnapshot& snapshot)
{
    if (!snapshot.visiblePath.empty()) {
        return fromWide(snapshot.visiblePath);
    }

    if (!snapshot.physicalDrivePath.empty()) {
        return fromWide(snapshot.physicalDrivePath);
    }

    return QStringLiteral("<pending-enumeration>");
}

} // namespace

Widget::Widget(Backend* backend, QWidget* parent)
    : QWidget(parent), ui(new Ui::Widget), backend(backend) {
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
    ui->sessionStateValueLabel->setText(
        backend != nullptr ? backend->sessionStateText()
                           : QStringLiteral("未接入宿主后端"));
    ui->createDiskButton->setEnabled(backend != nullptr);
    ui->removeDiskButton->setEnabled(false);

    if (backend != nullptr) {
        ui->logPlainTextEdit->setPlainText(
            backend->initialLogLines().join(QLatin1Char('\n')));
    } else {
        ui->logPlainTextEdit->setPlainText(
            QStringLiteral("[shell] backend placeholder missing"));
    }
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

void Widget::refreshView() {
    refreshSessionState();
    refreshManagedDisks();
    refreshLogLines();
}

void Widget::refreshSessionState() {
    ui->sessionStateValueLabel->setText(
        backend != nullptr ? backend->querySessionState()
                           : QStringLiteral("未接入宿主后端"));
}

void Widget::refreshManagedDisks() {
    const bool hadCurrentTarget = hasCurrentTarget();
    const unsigned long selectedTargetId =
        hadCurrentTarget ? currentTargetId() : 0;

    ui->diskTableWidget->setRowCount(0);
    if (backend == nullptr) {
        updateRemoveButtonState();
        return;
    }

    const auto snapshots = backend->snapshotManagedDisks();
    for (int rowIndex = 0; rowIndex < (int)snapshots.size(); ++rowIndex) {
        const auto& snapshot = snapshots[(size_t)rowIndex];
        auto* targetIdItem = new QTableWidgetItem(QString::number(snapshot.targetId));
        auto* lifecycleItem = new QTableWidgetItem(fromWide(snapshot.lifecycleText));
        auto* mediaItem = new QTableWidgetItem(mediaModeText(snapshot.mode));
        auto* visiblePathItem = new QTableWidgetItem(visiblePathText(snapshot));

        targetIdItem->setData(Qt::UserRole, QVariant::fromValue(snapshot.targetId));
        lifecycleItem->setData(Qt::UserRole, QVariant::fromValue(snapshot.targetId));
        mediaItem->setData(Qt::UserRole, QVariant::fromValue(snapshot.targetId));
        visiblePathItem->setData(Qt::UserRole, QVariant::fromValue(snapshot.targetId));

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

    updateRemoveButtonState();
}

void Widget::refreshLogLines() {
    if (backend == nullptr) {
        return;
    }

    const QString nextText = backend->logLines().join(QLatin1Char('\n'));
    if (ui->logPlainTextEdit->toPlainText() == nextText) {
        return;
    }

    ui->logPlainTextEdit->setPlainText(nextText);
    QTextCursor cursor = ui->logPlainTextEdit->textCursor();
    cursor.movePosition(QTextCursor::End);
    ui->logPlainTextEdit->setTextCursor(cursor);
}

void Widget::updateRemoveButtonState() {
    ui->removeDiskButton->setEnabled(
        (backend != nullptr) && hasCurrentTarget());
}

void Widget::createDisk() {
    if (backend == nullptr) {
        return;
    }

    CreateDiskDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QString errorText;
    if (!backend->createManagedDisk(dialog.createRequest(), &errorText)) {
        QMessageBox::warning(this, QStringLiteral("创建磁盘"), errorText);
        return;
    }

    refreshView();
}

void Widget::removeDisk() {
    if (backend == nullptr || !hasCurrentTarget()) {
        return;
    }

    QString errorText;
    if (!backend->removeManagedDisk(currentTargetId(), &errorText)) {
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
