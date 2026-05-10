#include "CreateDiskDialog.h"

#include "ui_CreateDiskDialog.h"

#include <QComboBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>

namespace {

BackendHostMediaMode mediaModeFromIndex(
    int index)
{
    switch (index) {
    case 1:
        return BackendHostMediaMode::denseMem;
    case 2:
        return BackendHostMediaMode::sparseMem;
    case 3:
        return BackendHostMediaMode::rawFile;
    default:
        return BackendHostMediaMode::autoSelect;
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
    connect(
        ui->mediaModeComboBox,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this]() {
            refreshModeUi();
        });
}

CreateDiskDialog::~CreateDiskDialog() {
    delete ui;
}

BackendHostCreateDiskRequest CreateDiskDialog::createRequest() const {
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
    ui->rawFileLineEdit->clear();
    ui->mediaModeComboBox->setCurrentIndex(0);
    ui->readOnlyCheckBox->setChecked(false);
    refreshModeUi();
}

void CreateDiskDialog::refreshModeUi() {
    const bool rawModeSelected = isRawModeSelected();

    ui->capacityLabel->setEnabled(!rawModeSelected);
    ui->capacityLineEdit->setEnabled(!rawModeSelected);
    ui->rawFileLabel->setEnabled(rawModeSelected);
    ui->rawFileLineEdit->setEnabled(rawModeSelected);
}

bool CreateDiskDialog::isRawModeSelected() const {
    return mediaModeFromIndex(ui->mediaModeComboBox->currentIndex()) ==
        BackendHostMediaMode::rawFile;
}

void CreateDiskDialog::submit() {
    acceptedRequest.capacityMiBText = ui->capacityLineEdit->text().trimmed();
    acceptedRequest.targetIdText = ui->targetIdLineEdit->text().trimmed();
    acceptedRequest.readOnly = ui->readOnlyCheckBox->isChecked();
    acceptedRequest.requestedMode = mediaModeFromIndex(ui->mediaModeComboBox->currentIndex());
    acceptedRequest.rawFilePath = ui->rawFileLineEdit->text().trimmed();
    accept();
}
