#include "createDiskDialog.h"

#include "ui_createDiskDialog.h"

#include <limits>

#include <QMessageBox>
#include <QRegularExpression>
#include <QRegularExpressionValidator>

namespace {

constexpr qulonglong mibBytes = 1024ull * 1024ull;
constexpr unsigned long maxTargetId = 255;

clientbackend::MediaMode mediaModeFromIndex(
    int index)
{
    switch (index) {
    case 1:
        return clientbackend::MediaMode::dense;
    case 2:
        return clientbackend::MediaMode::sparse;
    default:
        return clientbackend::MediaMode::autoSelect;
    }
}

} // namespace

CreateDiskDialog::CreateDiskDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::CreateDiskDialog)
{
    ui->setupUi(this);
    initializeUi();

    connect(ui->submitButton, &QPushButton::clicked, this, [this]() {
        submit();
    });
    connect(ui->cancelButton, &QPushButton::clicked, this, [this]() {
        reject();
    });
}

CreateDiskDialog::~CreateDiskDialog() {
    delete ui;
}

BackendCreateDiskRequest CreateDiskDialog::createRequest() const {
    return acceptedRequest;
}

void CreateDiskDialog::initializeUi() {
    const auto* digitsOnlyValidator = new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{0,20}")),
        this);

    setWindowTitle(QStringLiteral("Create Disk"));
    ui->capacityLineEdit->setValidator(digitsOnlyValidator);
    ui->targetIdLineEdit->setValidator(digitsOnlyValidator);
    ui->capacityLineEdit->setText(QStringLiteral("64"));
    ui->targetIdLineEdit->clear();
    ui->mediaModeComboBox->setCurrentIndex(0);
    ui->readOnlyCheckBox->setChecked(false);
}

bool CreateDiskDialog::tryBuildRequest(
    BackendCreateDiskRequest* outRequest,
    QString* outErrorText) const
{
    BackendCreateDiskRequest request;
    bool ok = false;
    const QString capacityText = ui->capacityLineEdit->text().trimmed();
    const QString targetIdText = ui->targetIdLineEdit->text().trimmed();
    const qulonglong maxCapacityMiB =
        std::numeric_limits<qulonglong>::max() / mibBytes;
    const qulonglong capacityMiB = capacityText.toULongLong(&ok);

    if (!ok || capacityMiB == 0) {
        if (outErrorText != nullptr) {
            *outErrorText = QStringLiteral("容量必须是大于 0 的 MiB 整数");
        }
        return false;
    }

    if (capacityMiB > maxCapacityMiB) {
        if (outErrorText != nullptr) {
            *outErrorText = QStringLiteral("容量超出当前支持范围");
        }
        return false;
    }

    request.diskSizeBytes = capacityMiB * mibBytes;
    request.readOnly = ui->readOnlyCheckBox->isChecked();
    request.requestedMode = mediaModeFromIndex(ui->mediaModeComboBox->currentIndex());

    if (!targetIdText.isEmpty()) {
        const qulonglong parsedTargetId = targetIdText.toULongLong(&ok);
        if (!ok || parsedTargetId > maxTargetId) {
            if (outErrorText != nullptr) {
                *outErrorText = QStringLiteral("target id 必须是 0 到 255 之间的整数");
            }
            return false;
        }

        request.targetId = static_cast<unsigned long>(parsedTargetId);
    }

    if (outRequest != nullptr) {
        *outRequest = request;
    }
    return true;
}

void CreateDiskDialog::submit() {
    QString errorText;
    BackendCreateDiskRequest request;

    if (!tryBuildRequest(&request, &errorText)) {
        QMessageBox::warning(this, QStringLiteral("创建磁盘"), errorText);
        return;
    }

    acceptedRequest = request;
    accept();
}
