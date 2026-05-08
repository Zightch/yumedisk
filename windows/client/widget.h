#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui {
class Widget;
}
QT_END_NAMESPACE

class QAction;
class QCloseEvent;
class QMenu;
class QSystemTrayIcon;

class Widget : public QWidget {
public:
    explicit Widget(QWidget* parent = nullptr);
    ~Widget() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void showFromTray();
    void initializeTray();

    Ui::Widget* ui;
    QSystemTrayIcon* trayIcon = nullptr;
    QMenu* trayMenu = nullptr;
    QAction* openAction = nullptr;
    QAction* quitAction = nullptr;
};
