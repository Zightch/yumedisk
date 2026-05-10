#pragma once

#include <QDialog>

#include "backendHost/BackendHost/BackendHost.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class CreateDiskDialog;
}
QT_END_NAMESPACE

class CreateDiskDialog : public QDialog {
public:
    explicit CreateDiskDialog(QWidget* parent = nullptr);
    ~CreateDiskDialog() override;

    BackendHostCreateDiskRequest createRequest() const;

private:
    void initializeUi();
    void refreshModeUi();
    bool isRawModeSelected() const;
    void submit();

    Ui::CreateDiskDialog* ui;
    BackendHostCreateDiskRequest acceptedRequest;
};
