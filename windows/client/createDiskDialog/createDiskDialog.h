#pragma once

#include <QDialog>

#include "backend/backend.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class CreateDiskDialog;
}
QT_END_NAMESPACE

class CreateDiskDialog : public QDialog {
public:
    explicit CreateDiskDialog(QWidget* parent = nullptr);
    ~CreateDiskDialog() override;

    BackendCreateDiskRequest createRequest() const;

private:
    void initializeUi();
    bool tryBuildRequest(
        BackendCreateDiskRequest* outRequest,
        QString* outErrorText) const;
    void submit();

    Ui::CreateDiskDialog* ui;
    BackendCreateDiskRequest acceptedRequest;
};
