// Copyright (c) 2011-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "editaddressdialog.h"
#include "ui_editaddressdialog.h"

#include "addresstablemodel.h"
#include "guiutil.h"

#include <QDataWidgetMapper>
#include <QMessageBox>

EditAddressDialog::EditAddressDialog(Mode _mode, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::EditAddressDialog),
    mapper(0),
    mode(_mode),
    model(0)
{
    ui->setupUi(this);

    GUIUtil::setupAddressWidget(ui->addressEdit, this);

    switch(mode)
    {
    case NewReceivingAddress:
        setWindowTitle(tr("New receiving address"));
        ui->addressEdit->setEnabled(false);
        break;
    case NewSendingAddress:
        setWindowTitle(tr("New sending address"));
        break;
    case EditReceivingAddress:
        setWindowTitle(tr("Edit receiving address"));
        ui->addressEdit->setEnabled(false);
        break;
    case EditSendingAddress:
        setWindowTitle(tr("Edit sending address"));
        break;
    }

    mapper = new QDataWidgetMapper(this);
    mapper->setSubmitPolicy(QDataWidgetMapper::ManualSubmit);
}

EditAddressDialog::~EditAddressDialog()
{
    delete ui;
}

void EditAddressDialog::setModel(ZCoinTableModel *_model)
{
    this->model = _model;
    if(!_model)
        return;
    
    mapper->setModel(_model);
    mapper->addMapping(ui->labelEdit, AddressTableModel::Label);
    mapper->addMapping(ui->addressEdit, AddressTableModel::Address);
}

void EditAddressDialog::loadRow(int row)
{
    mapper->setCurrentIndex(row);
}

void EditAddressDialog::setIsForAddress(bool val)
{
    if (mode != NewSendingAddress && mode != EditSendingAddress) return;
    this->isForAddress = val;
    if (!isForAddress) {
        //adding payment
        this->ui->label_2->setText("Payment Code");
        this->ui->addressEdit->setPlaceholderText("Enter a Zcoin payment code");
        if (mode == NewSendingAddress) {
            setWindowTitle(tr("New sending payment code"));
        } else {
            setWindowTitle(tr("Eding sending payment code"));
        }
    } 
}

bool EditAddressDialog::saveCurrentRow()
{
    if(!model)
        return false;

    switch(mode)
    {
    case NewReceivingAddress:
    case NewSendingAddress:
        if (isForAddress) {
            address = model->addRow(
                    mode == NewSendingAddress ? AddressTableModel::Send : AddressTableModel::Receive,
                    ui->labelEdit->text(),
                    ui->addressEdit->text());
        } else {
            address = model->addRow(
                    mode == NewSendingAddress ? PaymentCodeTableModel::Send : PaymentCodeTableModel::Receive,
                    ui->labelEdit->text(),
                    ui->addressEdit->text());
        }
        break;
    case EditReceivingAddress:
    case EditSendingAddress:
        if(mapper->submit())
        {
            address = ui->addressEdit->text();
        }
        break;
    }
    return !address.isEmpty();
}

void EditAddressDialog::accept()
{
    if(!model)
        return;

    if(!saveCurrentRow())
    {
        switch(model->getEditStatus())
        {
        case EditStatus::OK:
            // Failed with unknown reason. Just reject.
            break;
        case EditStatus::NO_CHANGES:
            // No changes were made during edit operation. Just reject.
            break;
        case EditStatus::INVALID_ADDRESS:
            QMessageBox::warning(this, windowTitle(),
                tr("The entered address \"%1\" is not a valid Zcoin address.").arg(ui->addressEdit->text()),
                QMessageBox::Ok, QMessageBox::Ok);
            break;
        case EditStatus::DUPLICATE_ADDRESS:
            QMessageBox::warning(this, windowTitle(),
                tr("The entered address \"%1\" is already in the address book.").arg(ui->addressEdit->text()),
                QMessageBox::Ok, QMessageBox::Ok);
            break;
        case EditStatus::INVALID_PAYMENTCODE:
            QMessageBox::warning(this, windowTitle(),
                tr("The entered payment code \"%1\" is not a valid Zcoin payment code.").arg(ui->addressEdit->text()),
                QMessageBox::Ok, QMessageBox::Ok);
            break;
        case EditStatus::DUPLICATE_PAYMENTCODE:
            QMessageBox::warning(this, windowTitle(),
                tr("The entered payment code \"%1\" is already in the payment code book.").arg(ui->addressEdit->text()),
                QMessageBox::Ok, QMessageBox::Ok);
            break;
        case EditStatus::WALLET_UNLOCK_FAILURE:
            QMessageBox::critical(this, windowTitle(),
                tr("Could not unlock wallet."),
                QMessageBox::Ok, QMessageBox::Ok);
            break;
        case EditStatus::KEY_GENERATION_FAILURE:
            QMessageBox::critical(this, windowTitle(),
                tr("New key generation failed."),
                QMessageBox::Ok, QMessageBox::Ok);
            break;

        }
        return;
    }
    QDialog::accept();
}

QString EditAddressDialog::getAddress() const
{
    return address;
}

void EditAddressDialog::setAddress(const QString &_address)
{
    this->address = _address;
    ui->addressEdit->setText(_address);
}
