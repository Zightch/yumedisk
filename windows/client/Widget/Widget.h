#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui {
class Widget;
}
QT_END_NAMESPACE

class QAction;
class Backend;
class QCloseEvent;
class QMenu;
class QSystemTrayIcon;
class QTimer;

class Widget : public QWidget {
public:
    explicit Widget(Backend* backend, QWidget* parent = nullptr);
    ~Widget() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void initializeShellUi();
    void initializeInteractions();
    void showFromTray();
    void initializeTray();
    void quitClient();
    void refreshView();
    void refreshSessionState();
    void refreshManagedDisks();
    void refreshLogLines();
    void updateRemoveButtonState();
    void createDisk();
    void removeDisk();
    unsigned long currentTargetId() const;
    bool hasCurrentTarget() const;

    Ui::Widget* ui;
    Backend* backend = nullptr;
    QSystemTrayIcon* trayIcon = nullptr;
    QMenu* trayMenu = nullptr;
    QAction* openAction = nullptr;
    QAction* quitAction = nullptr;
    QTimer* refreshTimer = nullptr;
};
