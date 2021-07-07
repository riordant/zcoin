// Copyright (c) 2011-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "walletmodel.h"

#include "addresstablemodel.h"
#include "consensus/validation.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "paymentserver.h"
#include "recentrequeststablemodel.h"
#include "recentpaymentcodetransactionstablemodel.h"
#include "transactiontablemodel.h"

#include "base58.h"
#include "keystore.h"
#include "validation.h"
#include "net.h" // for g_connman
#include "sync.h"
#include "ui_interface.h"
#include "util.h" // for GetBoolArg
#include "wallet/wallet.h"
#include "wallet/walletdb.h" // for BackupWallet
#include "wallet/walletexcept.h"
#include "txmempool.h"
#include "consensus/validation.h"
#include "sigma.h"
#include "sigma/coin.h"

#include "bip47/paymentcode.h"
#include "bip47/account.h"
#include "bip47/secretpoint.h"
#include "bip47/utils.h"

#include <stdint.h>

#include <QDebug>
#include <QSet>
#include <QTimer>

#include <boost/foreach.hpp>

WalletModel::WalletModel(const PlatformStyle *platformStyle, CWallet *_wallet, OptionsModel *_optionsModel, QObject *parent) :
    QObject(parent), wallet(_wallet), optionsModel(_optionsModel), addressTableModel(0),
    transactionTableModel(0),
    recentRequestsTableModel(0),
    recentPCodeTransactionsTableModel(0),
    paymentCodeTableModel(0),
    cachedBalance(0), cachedUnconfirmedBalance(0), cachedImmatureBalance(0),
    cachedEncryptionStatus(Unencrypted),
    cachedNumBlocks(0)
{
    fHaveWatchOnly = wallet->HaveWatchOnly();
    fForceCheckBalanceChanged = false;

    addressTableModel = new AddressTableModel(wallet, this);
    transactionTableModel = new TransactionTableModel(platformStyle, wallet, this);
    recentRequestsTableModel = new RecentRequestsTableModel(wallet, this);
    recentPCodeTransactionsTableModel = new RecentPCodeTransactionsTableModel(wallet, this);
    paymentCodeTableModel = new PaymentCodeTableModel(wallet, this);

    // This timer will be fired repeatedly to update the balance
    pollTimer = new QTimer(this);
    connect(pollTimer, SIGNAL(timeout()), this, SLOT(pollBalanceChanged()));
    pollTimer->start(MODEL_UPDATE_DELAY);

    subscribeToCoreSignals();
}

WalletModel::~WalletModel()
{
    unsubscribeFromCoreSignals();
}

CAmount WalletModel::getBalance(const CCoinControl *coinControl, bool fExcludeLocked) const
{
    if (coinControl)
    {
        CAmount nBalance = 0;
        std::vector<COutput> vCoins;
        wallet->AvailableCoins(vCoins, true, coinControl);
        BOOST_FOREACH(const COutput& out, vCoins)
            if(out.fSpendable)
                nBalance += out.tx->tx->vout[out.i].nValue;

        return nBalance;
    }

    return wallet->GetBalance(fExcludeLocked);
}

CAmount WalletModel::getUnconfirmedBalance() const
{
    return wallet->GetUnconfirmedBalance();
}

CAmount WalletModel::getImmatureBalance() const
{
    return wallet->GetImmatureBalance();
}

bool WalletModel::haveWatchOnly() const
{
    return fHaveWatchOnly;
}

CAmount WalletModel::getWatchBalance() const
{
    return wallet->GetWatchOnlyBalance();
}

CAmount WalletModel::getWatchUnconfirmedBalance() const
{
    return wallet->GetUnconfirmedWatchOnlyBalance();
}

CAmount WalletModel::getWatchImmatureBalance() const
{
    return wallet->GetImmatureWatchOnlyBalance();
}

void WalletModel::updateStatus()
{
    EncryptionStatus newEncryptionStatus = getEncryptionStatus();

    if(cachedEncryptionStatus != newEncryptionStatus)
        Q_EMIT encryptionStatusChanged(newEncryptionStatus);
}

void WalletModel::pollBalanceChanged()
{
    // Get required locks upfront. This avoids the GUI from getting stuck on
    // periodical polls if the core is holding the locks for a longer time -
    // for example, during a wallet rescan.
    TRY_LOCK(cs_main, lockMain);
    if(!lockMain)
        return;
    TRY_LOCK(wallet->cs_wallet, lockWallet);
    if(!lockWallet)
        return;

    if(fForceCheckBalanceChanged || chainActive.Height() != cachedNumBlocks)
    {
        fForceCheckBalanceChanged = false;

        // Balance and number of transactions might have changed
        cachedNumBlocks = chainActive.Height();

        checkBalanceChanged();
        if(transactionTableModel)
            transactionTableModel->updateConfirmations();

        // check sigma
        // support only hd
        if (wallet->zwallet) {
            checkSigmaAmount(false);
        }
    }
}

void WalletModel::updateSigmaCoins(const QString &pubCoin, const QString &isUsed, int status)
{
    if (status == ChangeType::CT_UPDATED) {
        // some coin have been updated to be used
        LOCK2(cs_main, wallet->cs_wallet);
        checkSigmaAmount(true);

    } else if (status == ChangeType::CT_NEW) {
        // new mint
        LOCK2(cs_main, wallet->cs_wallet);
        auto coins = wallet->zwallet->GetTracker().ListMints(true, false, false);

        int block = cachedNumBlocks;
        for (const auto& coin : coins) {
            if (!coin.isUsed) {
                int coinHeight = coin.nHeight;
                if (coinHeight == -1
                    || (coinHeight <= block && coinHeight > block - ZC_MINT_CONFIRMATIONS)) {
                    cachedHavePendingCoin = true;
                }
            }
        }

        if (cachedHavePendingCoin) {
            checkSigmaAmount(true);
        }
    }
}

void WalletModel::checkBalanceChanged()
{
    CAmount newBalance = getBalance();
    CAmount newUnconfirmedBalance = getUnconfirmedBalance();
    CAmount newImmatureBalance = getImmatureBalance();
    CAmount newWatchOnlyBalance = 0;
    CAmount newWatchUnconfBalance = 0;
    CAmount newWatchImmatureBalance = 0;
    if (haveWatchOnly())
    {
        newWatchOnlyBalance = getWatchBalance();
        newWatchUnconfBalance = getWatchUnconfirmedBalance();
        newWatchImmatureBalance = getWatchImmatureBalance();
    }

    if(cachedBalance != newBalance || cachedUnconfirmedBalance != newUnconfirmedBalance || cachedImmatureBalance != newImmatureBalance ||
        cachedWatchOnlyBalance != newWatchOnlyBalance || cachedWatchUnconfBalance != newWatchUnconfBalance || cachedWatchImmatureBalance != newWatchImmatureBalance)
    {
        cachedBalance = newBalance;
        cachedUnconfirmedBalance = newUnconfirmedBalance;
        cachedImmatureBalance = newImmatureBalance;
        cachedWatchOnlyBalance = newWatchOnlyBalance;
        cachedWatchUnconfBalance = newWatchUnconfBalance;
        cachedWatchImmatureBalance = newWatchImmatureBalance;
        Q_EMIT balanceChanged(newBalance, newUnconfirmedBalance, newImmatureBalance,
                            newWatchOnlyBalance, newWatchUnconfBalance, newWatchImmatureBalance);
    }
}

void WalletModel::checkSigmaAmount(bool forced)
{
    auto currentBlock = chainActive.Height();
    if ((cachedHavePendingCoin && currentBlock > lastBlockCheckSigma)
        || currentBlock < lastBlockCheckSigma // reorg
        || forced) {

        auto coins = wallet->zwallet->GetTracker().ListMints(true, false, false);

        std::vector<CMintMeta> spendable, pending;

        std::vector<sigma::PublicCoin> anonimity_set;
        uint256 blockHash;

        cachedHavePendingCoin = false;

        for (const auto& coin : coins) {

            // ignore spent coin
            if (coin.isUsed)
                continue;

            int coinHeight = coin.nHeight;

            if (coinHeight > 0
                && coinHeight + (ZC_MINT_CONFIRMATIONS-1) <= chainActive.Height())  {
                spendable.push_back(coin);
            } else {
                cachedHavePendingCoin = true;
                pending.push_back(coin);
            }
        }

        lastBlockCheckSigma = currentBlock;
        Q_EMIT notifySigmaChanged(spendable, pending);
    }
}

void WalletModel::updateTransaction()
{
    // Balance and number of transactions might have changed
    fForceCheckBalanceChanged = true;
}

void WalletModel::updateAddressBook(const QString &address, const QString &label,
        bool isMine, const QString &purpose, int status)
{
    if(addressTableModel)
        addressTableModel->updateEntry(address, label, isMine, purpose, status);
}

void WalletModel::updateAddressBook(const QString &pubCoin, const QString &isUsed, int status)
{
    if(addressTableModel)
        addressTableModel->updateEntry(pubCoin, isUsed, status);
}

void WalletModel::updateWatchOnlyFlag(bool fHaveWatchonly)
{
    fHaveWatchOnly = fHaveWatchonly;
    Q_EMIT notifyWatchonlyChanged(fHaveWatchonly);
}

bool WalletModel::tryEnablePaymentCode()
{
    if(wallet->pcodeEnabled && wallet->m_Bip47PendingKeys.empty())
        return true;
    if(wallet->IsLocked())
    {
        QMessageBox::information(0, tr("Unlock Wallet"), tr("To Receive PaymentCode Transaction You need to Unlock Wallet"));
        WalletModel::UnlockContext ctx(requestUnlock());
        if(!ctx.isValid())
        {
            // Unlock wallet was cancelled
            return false;
        }
        if(wallet->pcodeEnabled)
        {
            wallet->importBip47PendingKeys();
            return true;
        }
        CExtKey masterKey;
        if(wallet->ReadMasterKey(masterKey))
        {
            wallet->loadBip47Wallet(masterKey);
            wallet->pcodeEnabled = true;
            return true;
        }
        else
        {
            LogPrintf("Pcode disabled\n");
            wallet->pcodeEnabled = false;
            return false;
        }
    }
    return true;
}

bool WalletModel::validateAddress(const QString &address)
{
    CBitcoinAddress addressParsed(address.toStdString());
    return addressParsed.IsValid();
}

bool WalletModel::validatePaymentCode(const QString &pCode)
{
    CPaymentCode paymentCode(pCode.toStdString());
    return paymentCode.isValid();
}

bool WalletModel::isNotificationTransactionSent(const QString &pCode) const
{
    return wallet->isNotificationTransactionSent(pCode.toStdString());
}

WalletModel::SendCoinsReturn WalletModel::prepareTransaction(WalletModelTransaction &transaction, const CCoinControl *coinControl)
{
    CAmount total = 0;
    bool fSubtractFeeFromAmount = false;
    QList<SendCoinsRecipient> recipients = transaction.getRecipients();
    std::vector<CRecipient> vecSend;

    if(recipients.empty())
    {
        return OK;
    }

    QSet<QString> setAddress; // Used to detect duplicates
    int nAddresses = 0;

    // Pre-check input data for validity
    Q_FOREACH(const SendCoinsRecipient &rcp, recipients)
    {
        if (rcp.fSubtractFeeFromAmount)
            fSubtractFeeFromAmount = true;

        if (rcp.paymentRequest.IsInitialized())
        {   // PaymentRequest...
            CAmount subtotal = 0;
            const payments::PaymentDetails& details = rcp.paymentRequest.getDetails();
            for (int i = 0; i < details.outputs_size(); i++)
            {
                const payments::Output& out = details.outputs(i);
                if (out.amount() <= 0) continue;
                subtotal += out.amount();
                const unsigned char* scriptStr = (const unsigned char*)out.script().data();
                CScript scriptPubKey(scriptStr, scriptStr+out.script().size());
                CAmount nAmount = out.amount();
                CRecipient recipient = {scriptPubKey, nAmount, rcp.fSubtractFeeFromAmount};
                vecSend.push_back(recipient);
            }
            if (subtotal <= 0)
            {
                return InvalidAmount;
            }
            total += subtotal;
        }
        else
        {   // User-entered Zcoin address / amount:
            if(!validateAddress(rcp.address))
            {
                return InvalidAddress;
            }
            if(rcp.amount <= 0)
            {
                return InvalidAmount;
            }
            setAddress.insert(rcp.address);
            ++nAddresses;

            CScript scriptPubKey = GetScriptForDestination(CBitcoinAddress(rcp.address.toStdString()).Get());
            CRecipient recipient = {scriptPubKey, rcp.amount, rcp.fSubtractFeeFromAmount};
            vecSend.push_back(recipient);

            total += rcp.amount;
        }
    }
    if(setAddress.size() != nAddresses)
    {
        return DuplicateAddress;
    }

    CAmount nBalance = getBalance(coinControl);

    if(total > nBalance)
    {
        return AmountExceedsBalance;
    }

    {
        LOCK2(cs_main, wallet->cs_wallet);

        transaction.newPossibleKeyChange(wallet);

        CAmount nFeeRequired = 0;
        int nChangePosRet = -1;
        std::string strFailReason;

        CWalletTx *newTx = transaction.getTransaction();
        CReserveKey *keyChange = transaction.getPossibleKeyChange();
        bool fCreated = wallet->CreateTransaction(vecSend, *newTx, *keyChange, nFeeRequired, nChangePosRet, strFailReason, coinControl);
        transaction.setTransactionFee(nFeeRequired);
        if (fSubtractFeeFromAmount && fCreated)
            transaction.reassignAmounts(nChangePosRet);

        if(!fCreated)
        {
            if(!fSubtractFeeFromAmount && (total + nFeeRequired) > nBalance)
            {
                return SendCoinsReturn(AmountWithFeeExceedsBalance);
            }
            Q_EMIT message(tr("Send Coins"), QString::fromStdString(strFailReason),
                         CClientUIInterface::MSG_ERROR);
            return TransactionCreationFailed;
        }

        // reject absurdly high fee. (This can never happen because the
        // wallet caps the fee at maxTxFee. This merely serves as a
        // belt-and-suspenders check)
        if (nFeeRequired > maxTxFee)
            return AbsurdFee;
    }

    return SendCoinsReturn(OK);
}

WalletModel::SendCoinsReturn WalletModel::sendCoins(WalletModelTransaction &transaction)
{
    QByteArray transaction_array; /* store serialized transaction */

    {
        LOCK2(cs_main, wallet->cs_wallet);
        CWalletTx *newTx = transaction.getTransaction();

        Q_FOREACH(const SendCoinsRecipient &rcp, transaction.getRecipients())
        {
            if (rcp.paymentRequest.IsInitialized())
            {
                // Make sure any payment requests involved are still valid.
                if (PaymentServer::verifyExpired(rcp.paymentRequest.getDetails())) {
                    return PaymentRequestExpired;
                }

                // Store PaymentRequests in wtx.vOrderForm in wallet.
                std::string key("PaymentRequest");
                std::string value;
                rcp.paymentRequest.SerializeToString(&value);
                newTx->vOrderForm.push_back(make_pair(key, value));
            }
            else if (!rcp.message.isEmpty()) // Message from normal zcoin:URI (zcoin:123...?message=example)
                newTx->vOrderForm.push_back(make_pair("Message", rcp.message.toStdString()));
        }

        CReserveKey *keyChange = transaction.getPossibleKeyChange();
        CValidationState state;
        if(!wallet->CommitTransaction(*newTx, *keyChange, g_connman.get(), state))
            return SendCoinsReturn(TransactionCommitFailed, QString::fromStdString(state.GetRejectReason()));

        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << *newTx->tx;
        transaction_array.append(&(ssTx[0]), ssTx.size());
    }

    // Add addresses / update labels that we've sent to to the address book,
    // and emit coinsSent signal for each recipient
    Q_FOREACH(const SendCoinsRecipient &rcp, transaction.getRecipients())
    {
        // Don't touch the address book when we have a payment request
        if (!rcp.paymentRequest.IsInitialized())
        {
            std::string strAddress = rcp.address.toStdString();
            CTxDestination dest = CBitcoinAddress(strAddress).Get();
            std::string strLabel = rcp.label.toStdString();
            {
                LOCK(wallet->cs_wallet);

                std::map<CTxDestination, CAddressBookData>::iterator mi = wallet->mapAddressBook.find(dest);

                // Check if we have a new address or an updated label
                if (mi == wallet->mapAddressBook.end())
                {
                    wallet->SetAddressBook(dest, strLabel, "send");
                }
                else if (mi->second.name != strLabel)
                {
                    wallet->SetAddressBook(dest, strLabel, ""); // "" means don't change purpose
                }
            }
        }
        Q_EMIT coinsSent(wallet, rcp, transaction_array);
    }
    checkBalanceChanged(); // update balance immediately, otherwise there could be a short noticeable delay until pollBalanceChanged hits

    return SendCoinsReturn(OK);
}

WalletModel::SendCoinsReturn WalletModel::preparePCodeTransaction(WalletModelTransaction &transaction, const CCoinControl *coinControl)
{
    CAmount total = 0;
    bool fSubtractFeeFromAmount = false;
    bool isNotificationTx = true;
    QList<SendCoinsRecipient> recipients = transaction.getRecipients();
    std::vector<CRecipient> vecSend;

    if(recipients.empty())
    {
        return OK;
    }

    QSet<QString> setAddress; // Used to detect duplicates
    int nAddresses = 0;



    CBIP47Account toCBIP47Account;

    // Pre-check input data for validity
    Q_FOREACH(const SendCoinsRecipient &rcp, recipients)
    {
        if (rcp.fSubtractFeeFromAmount)
            fSubtractFeeFromAmount = true;

        // User-entered Zcoin address / amount:
        if(!validatePaymentCode(rcp.address))
        {
            return InvalidAddress;
        }
        if(rcp.amount <= 0)
        {
            return InvalidAmount;
        }

        // Get Or Create Payment Channel from payment code lgtm [cpp/commented-out-code];
        CBIP47PaymentChannel* channel = wallet->getPaymentChannelFromPaymentCode(rcp.address.toStdString());

        // If channel already sent notifcation transaction. lgtm [cpp/commented-out-code]
        if (channel->isNotificationTransactionSent()) {
            LogPrintf("Payment Notification Transaction Already Sent\n");
            isNotificationTx = false;
            std::string addressTo = wallet->getCurrentOutgoingAddress(*channel);

            setAddress.insert(rcp.address);
            ++nAddresses;
            CBitcoinAddress pcAddress(addressTo);

            CScript scriptPubKey = GetScriptForDestination(pcAddress.Get());
            CRecipient recipient = {scriptPubKey, rcp.amount, rcp.fSubtractFeeFromAmount};
            vecSend.push_back(recipient);


            total += rcp.amount;
        }
        else
        {
            LogPrintf("Payment Notification Transaction Still Not Sent\n");
            isNotificationTx = true;

            toCBIP47Account.SetPaymentCodeString(rcp.address.toStdString());

            CBitcoinAddress ntAddress = toCBIP47Account.getNotificationAddress();



            setAddress.insert(rcp.address);
            ++nAddresses;

            CScript scriptPubKey = GetScriptForDestination(ntAddress.Get());
            CRecipient recipient = {scriptPubKey, CENT / 2, rcp.fSubtractFeeFromAmount};
            vecSend.push_back(recipient);


            total +=  CENT / 2;
        }




    }
    if(setAddress.size() != nAddresses)
    {
        return DuplicateAddress;
    }

    CAmount nBalance = getBalance(coinControl);

    if(total > nBalance)
    {
        return AmountExceedsBalance;
    }

    {
        LOCK2(cs_main, wallet->cs_wallet);

        transaction.newPossibleKeyChange(wallet);

        CAmount nFeeRequired = 0;
        int nChangePosRet = -1;
        std::string strFailReason;

        CWalletTx *newTx = transaction.getTransaction();
        
        CReserveKey *keyChange = transaction.getPossibleKeyChange();
        bool fCreated = wallet->CreateTransaction(vecSend, *newTx, *keyChange, nFeeRequired, nChangePosRet, strFailReason, coinControl);

        // Notification Transaction setup here.
        if(isNotificationTx) {
            CPubKey designatedPubKey;

            CKey privKey;

            vector<unsigned char> pubKeyBytes;
            
            if (!CBIP47Util::getScriptSigPubkey(newTx->tx->vin[0], pubKeyBytes))
            {
                throw std::runtime_error("Bip47Utiles PaymentCode ScriptSig GetPubkey error\n");
            }
            else
            {

                designatedPubKey.Set(pubKeyBytes.begin(), pubKeyBytes.end());
                LogPrintf("ScriptSigPubKey Hash %s\n", designatedPubKey.GetHash().GetHex());

            }


            
            wallet->GetKey(designatedPubKey.GetID(), privKey);
            CPubKey pubkey = toCBIP47Account.getNotificationKey().pubkey;
            vector<unsigned char> dataPriv(privKey.size());
            vector<unsigned char> dataPub(pubkey.size());

            CBIP47Util::arraycopy(privKey.begin(), 0, dataPriv, 0, privKey.size());
            CBIP47Util::arraycopy(pubkey.begin(), 0, dataPub, 0, pubkey.size());

            LogPrintf("Generate Secret Point\n");
            SecretPoint secretPoint(dataPriv, dataPub);
            LogPrintf("Generating Secret Point with \n privekey: %s\n pubkey: %s\n", HexStr(dataPriv), HexStr(dataPub));
            
            vector<unsigned char> outpoint(newTx->tx->vin[0].prevout.hash.begin(), newTx->tx->vin[0].prevout.hash.end());

            LogPrintf("output: %s\n", newTx->tx->vin[0].prevout.hash.GetHex());
            uint256 secretPBytes(secretPoint.ECDHSecretAsBytes());
            LogPrintf("secretPoint: %s\n", secretPBytes.GetHex());

            LogPrintf("Get Mask from payment code\n");
            vector<unsigned char> mask = CPaymentCode::getMask(secretPoint.ECDHSecretAsBytes(), outpoint);

            LogPrintf("Get op_return bytes via blind\n");
            vector<unsigned char> op_return = CPaymentCode::blind(pwalletMain->getBIP47Account(0).getPaymentCode().getPayload(), mask);

            CScript op_returnScriptPubKey = CScript() << OP_RETURN << op_return;
            CRecipient pcodeBlind = {op_returnScriptPubKey, 0, false};
            LogPrintf("Add Blind Code to vecSend\n");
            vecSend.push_back(pcodeBlind);

            fCreated = wallet->CreateTransaction(vecSend, *newTx, *keyChange, nFeeRequired, nChangePosRet, strFailReason, coinControl);
            
            if (!CBIP47Util::getScriptSigPubkey(newTx->tx->vin[0], pubKeyBytes))
            {
                throw std::runtime_error("Bip47Utiles PaymentCode ScriptSig GetPubkey error\n");
            }
            else
            {

                designatedPubKey.Set(pubKeyBytes.begin(), pubKeyBytes.end());
                LogPrintf("ScriptSigPubKey Hash %s\n", designatedPubKey.GetHash().GetHex());
                if(!privKey.VerifyPubKey(designatedPubKey))
                {
                    throw std::runtime_error("Bip47Utiles PaymentCode ScriptSig designatedPubKey cannot be verified \n");
                }

            }
        }


        transaction.setTransactionFee(nFeeRequired);
        if (fSubtractFeeFromAmount && fCreated)
            transaction.reassignAmounts(nChangePosRet);

        if(!fCreated)
        {
            if(!fSubtractFeeFromAmount && (total + nFeeRequired) > nBalance)
            {
                return SendCoinsReturn(AmountWithFeeExceedsBalance);
            }
            if(strFailReason.compare("Insufficient funds.") == 0)
            {
               strFailReason = "You don't have enough of a confirmed balance to send the next transaction, please wait a few confirmations, then attempt to send the transaction again";
            }
            Q_EMIT message(tr("Send Coins"), QString::fromStdString(strFailReason),
                           CClientUIInterface::MSG_ERROR);
            return TransactionCreationFailed;
        }

        // reject absurdly high fee. (This can never happen because the
        // wallet caps the fee at maxTxFee. This merely serves as a
        // belt-and-suspenders check)
        if (nFeeRequired > maxTxFee)
            return AbsurdFee;
    }

    return SendCoinsReturn(OK);
}

WalletModel::SendCoinsReturn WalletModel::sendPCodeCoins(WalletModelTransaction &transaction, bool &needMainTx)
{
    
    needMainTx = false;
    QByteArray transaction_array; /* store serialized transaction */

    {
        LOCK2(cs_main, wallet->cs_wallet);
        CWalletTx *newTx = transaction.getTransaction();

        Q_FOREACH(const SendCoinsRecipient &rcp, transaction.getRecipients())
        {
            if (!rcp.message.isEmpty())
                newTx->vOrderForm.push_back(make_pair("Message", rcp.message.toStdString()));
        }

        CReserveKey *keyChange = transaction.getPossibleKeyChange();
        CValidationState state;
        if(!wallet->CommitTransaction(*newTx, *keyChange, g_connman.get(), state))
            return TransactionCommitFailed;

        const CTransaction* t = (newTx->tx.get());
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << *t;
        transaction_array.append(&(ssTx[0]), ssTx.size());
    }

    // Add addresses / update labels that we've sent to to the address book,
    // and emit coinsSent signal for each recipient
    Q_FOREACH(const SendCoinsRecipient &rcp, transaction.getRecipients())
    {
        std::string pcodestr = rcp.address.toStdString();
        std::string strLabel = rcp.label.toStdString();
        CBIP47PaymentChannel pchannel(pcodestr, strLabel);

        // Add to payment channel return true or false lgtm [cpp/commented-out-code] ;
        wallet->addToCBIP47PaymentChannel(pchannel);
        CBIP47PaymentChannel* channel = wallet->getPaymentChannelFromPaymentCode(pcodestr);
        channel->setLabel(strLabel);
        if(!channel->isNotificationTransactionSent())
        {
            channel->setStatusSent(transaction.getTransaction()->GetHash());
            needMainTx = true;
        }
        else 
        {
            std::string pcoutaddress = wallet->getCurrentOutgoingAddress(*channel);
            channel->addAddressToOutgoingAddresses(pcoutaddress);
            channel->incrementOutgoingIndex();
            
        }
        wallet->saveCBIP47PaymentChannelData(pcodestr);
        paymentCodeTableModel->refreshModel();

        Q_EMIT coinsSent(wallet, rcp, transaction_array);
    }
    checkBalanceChanged(); // update balance immediately, otherwise there could be a short noticeable delay until pollBalanceChanged hits

    return SendCoinsReturn(OK);
}



OptionsModel *WalletModel::getOptionsModel()
{
    return optionsModel;
}

AddressTableModel *WalletModel::getAddressTableModel()
{
    return addressTableModel;
}

PaymentCodeTableModel *WalletModel::getPaymentCodeTableModel()
{
    return paymentCodeTableModel;
}

TransactionTableModel *WalletModel::getTransactionTableModel()
{
    return transactionTableModel;
}

RecentRequestsTableModel *WalletModel::getRecentRequestsTableModel()
{
    return recentRequestsTableModel;
}

RecentPCodeTransactionsTableModel *WalletModel::getRecentPCodeTransactionsTableModel()
{
    return recentPCodeTransactionsTableModel;
}

WalletModel::EncryptionStatus WalletModel::getEncryptionStatus() const
{
    if(!wallet->IsCrypted())
    {
        return Unencrypted;
    }
    else if(wallet->IsLocked())
    {
        return Locked;
    }
    else
    {
        return Unlocked;
    }
}

bool WalletModel::setWalletEncrypted(bool encrypted, const SecureString &passphrase)
{
    if(encrypted)
    {
        // Encrypt
        return wallet->EncryptWallet(passphrase);
    }
    else
    {
        // Decrypt -- TODO; not supported yet
        return false;
    }
}

bool WalletModel::setWalletLocked(bool locked, const SecureString &passPhrase)
{
    if(locked)
    {
        // Lock
        return wallet->Lock();
    }
    else
    {
        // Unlock
        return wallet->Unlock(passPhrase);
    }
}

bool WalletModel::changePassphrase(const SecureString &oldPass, const SecureString &newPass)
{
    bool retval;
    {
        LOCK(wallet->cs_wallet);
        wallet->Lock(); // Make sure wallet is locked before attempting pass change
        retval = wallet->ChangeWalletPassphrase(oldPass, newPass);
    }
    return retval;
}

bool WalletModel::backupWallet(const QString &filename)
{
    return wallet->BackupWallet(filename.toLocal8Bit().data());
}

bool WalletModel::backupBip47Wallet(const QString &filename)
{
    return wallet->BackupBip47Wallet(filename.toLocal8Bit().data());
}

// Handlers for core signals
static void NotifyKeyStoreStatusChanged(WalletModel *walletmodel, CCryptoKeyStore *wallet)
{
    qDebug() << "NotifyKeyStoreStatusChanged";
    QMetaObject::invokeMethod(walletmodel, "updateStatus", Qt::QueuedConnection);
}

static void NotifyAddressBookChanged(WalletModel *walletmodel, CWallet *wallet,
        const CTxDestination &address, const std::string &label, bool isMine,
        const std::string &purpose, ChangeType status)
{
    QString strAddress = QString::fromStdString(CBitcoinAddress(address).ToString());
    QString strLabel = QString::fromStdString(label);
    QString strPurpose = QString::fromStdString(purpose);

    qDebug() << "NotifyAddressBookChanged: " + strAddress + " " + strLabel + " isMine=" + QString::number(isMine) + " purpose=" + strPurpose + " status=" + QString::number(status);
    QMetaObject::invokeMethod(walletmodel, "updateAddressBook", Qt::QueuedConnection,
                              Q_ARG(QString, strAddress),
                              Q_ARG(QString, strLabel),
                              Q_ARG(bool, isMine),
                              Q_ARG(QString, strPurpose),
                              Q_ARG(int, status));
}

static void NotifyZerocoinChanged(WalletModel *walletmodel, CWallet *wallet, const std::string &pubCoin, const std::string &isUsed, ChangeType status)
{
    qDebug() << "NotifyZerocoinChanged:" + QString::fromStdString(pubCoin) + " " + QString::fromStdString(isUsed) + " status=" + QString::number(status);
    QMetaObject::invokeMethod(walletmodel, "updateAddressBook", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(pubCoin)),
                              Q_ARG(QString, QString::fromStdString(isUsed)),
                              Q_ARG(int, status));

    // disable sigma
    if (wallet->zwallet) {
        QMetaObject::invokeMethod(walletmodel, "updateSigmaCoins", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(pubCoin)),
                              Q_ARG(QString, QString::fromStdString(isUsed)),
                              Q_ARG(int, status));
    }
}

static void NotifyTransactionChanged(WalletModel *walletmodel, CWallet *wallet, const uint256 &hash, ChangeType status)
{
    Q_UNUSED(wallet);
    Q_UNUSED(hash);
    Q_UNUSED(status);
    QMetaObject::invokeMethod(walletmodel, "updateTransaction", Qt::QueuedConnection);
}

static void NotifyPaymentCodeTx(WalletModel *walletmodel)
{
    LogPrintf("Get NotifyPaymentCodeTx\n");
    QMetaObject::invokeMethod(walletmodel, "tryEnablePaymentCode", Qt::QueuedConnection);
}



static void ShowProgress(WalletModel *walletmodel, const std::string &title, int nProgress)
{
    // emits signal "showProgress"
    QMetaObject::invokeMethod(walletmodel, "showProgress", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(title)),
                              Q_ARG(int, nProgress));
}

static void NotifyWatchonlyChanged(WalletModel *walletmodel, bool fHaveWatchonly)
{
    QMetaObject::invokeMethod(walletmodel, "updateWatchOnlyFlag", Qt::QueuedConnection,
                              Q_ARG(bool, fHaveWatchonly));
}

void WalletModel::subscribeToCoreSignals()
{
    // Connect signals to wallet
    wallet->NotifyStatusChanged.connect(boost::bind(&NotifyKeyStoreStatusChanged, this, _1));
    wallet->NotifyPaymentCodeTx.connect(boost::bind(&NotifyPaymentCodeTx, this));
    wallet->NotifyAddressBookChanged.connect(boost::bind(NotifyAddressBookChanged, this, _1, _2, _3, _4, _5, _6));
    wallet->NotifyTransactionChanged.connect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
    wallet->ShowProgress.connect(boost::bind(ShowProgress, this, _1, _2));
    wallet->NotifyWatchonlyChanged.connect(boost::bind(NotifyWatchonlyChanged, this, _1));
    wallet->NotifyZerocoinChanged.connect(boost::bind(NotifyZerocoinChanged, this, _1, _2, _3, _4));
}

void WalletModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    wallet->NotifyStatusChanged.disconnect(boost::bind(&NotifyKeyStoreStatusChanged, this, _1));
    wallet->NotifyPaymentCodeTx.disconnect(boost::bind(&NotifyPaymentCodeTx, this));
    wallet->NotifyAddressBookChanged.disconnect(boost::bind(NotifyAddressBookChanged, this, _1, _2, _3, _4, _5, _6));
    wallet->NotifyTransactionChanged.disconnect(boost::bind(NotifyTransactionChanged, this, _1, _2, _3));
    wallet->ShowProgress.disconnect(boost::bind(ShowProgress, this, _1, _2));
    wallet->NotifyWatchonlyChanged.disconnect(boost::bind(NotifyWatchonlyChanged, this, _1));
    wallet->NotifyZerocoinChanged.disconnect(boost::bind(NotifyZerocoinChanged, this, _1, _2, _3, _4));
}

// WalletModel::UnlockContext implementation
WalletModel::UnlockContext WalletModel::requestUnlock()
{
    bool was_locked = getEncryptionStatus() == Locked;
    if(was_locked)
    {
        // Request UI to unlock wallet
        Q_EMIT requireUnlock();
    }
    // If wallet is still locked, unlock was failed or cancelled, mark context as invalid
    bool valid = getEncryptionStatus() != Locked;

    return UnlockContext(this, valid, was_locked);
}

WalletModel::UnlockContext::UnlockContext(WalletModel *_wallet, bool _valid, bool _relock):
        wallet(_wallet),
        valid(_valid),
        relock(_relock)
{
}

WalletModel::UnlockContext::~UnlockContext()
{
    if(valid && relock)
    {
        wallet->setWalletLocked(true);
    }
}

void WalletModel::UnlockContext::CopyFrom(const UnlockContext& rhs)
{
    // Transfer context; old object no longer relocks wallet
    *this = rhs;
    rhs.relock = false;
}

bool WalletModel::IsSpendable(const CTxDestination& dest) const
{
    return IsMine(*wallet, dest) & ISMINE_SPENDABLE;
}

bool WalletModel::IsSpendable(const CScript& script) const
{
    return IsMine(*wallet, script) & ISMINE_SPENDABLE;
}

bool WalletModel::getPubKey(const CKeyID &address, CPubKey& vchPubKeyOut) const
{
    return wallet->GetPubKey(address, vchPubKeyOut);
}

bool WalletModel::havePrivKey(const CKeyID &address) const
{
    return wallet->HaveKey(address);
}

bool WalletModel::getPrivKey(const CKeyID &address, CKey& vchPrivKeyOut) const
{
    return wallet->GetKey(address, vchPrivKeyOut);
}

// returns a list of COutputs from COutPoints
void WalletModel::getOutputs(const std::vector<COutPoint>& vOutpoints, std::vector<COutput>& vOutputs, boost::optional<bool> fMintTabSelected)
{
    LOCK2(cs_main, wallet->cs_wallet);
    BOOST_FOREACH(const COutPoint& outpoint, vOutpoints)
    {
        if (!wallet->mapWallet.count(outpoint.hash)) continue;
        int nDepth = wallet->mapWallet[outpoint.hash].GetDepthInMainChain();
        if (nDepth < 0) continue;
        if (fMintTabSelected != boost::none) {
            if(wallet->mapWallet[outpoint.hash].tx->vout[outpoint.n].scriptPubKey.IsSigmaMint()) {
                if (fMintTabSelected.get()) // only allow mint outputs on the "Spend" tab
                    continue;
            }
            else {
                if (!fMintTabSelected.get())
                    continue; // only allow normal outputs on the "Mint" tab
            }
        }
        COutput out(&wallet->mapWallet[outpoint.hash], outpoint.n, nDepth, true, true);
        vOutputs.push_back(out);
    }
}

bool WalletModel::isSpent(const COutPoint& outpoint) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->IsSpent(outpoint.hash, outpoint.n);
}

// AvailableCoins + LockedCoins grouped by wallet address (put change in one group with wallet address)
void WalletModel::listCoins(std::map<QString, std::vector<COutput> >& mapCoins, AvailableCoinsType nCoinType) const
{
    std::vector<COutput> vCoins;
    wallet->AvailableCoins(vCoins, true, NULL, false, nCoinType, false);

    LOCK2(cs_main, wallet->cs_wallet); // ListLockedCoins, mapWallet
    std::vector<COutPoint> vLockedCoins;
    wallet->ListLockedCoins(vLockedCoins);

    // add locked coins
    BOOST_FOREACH(const COutPoint& outpoint, vLockedCoins)
    {
        if (!wallet->mapWallet.count(outpoint.hash)) continue;
        int nDepth = wallet->mapWallet[outpoint.hash].GetDepthInMainChain();
        if (nDepth < 0) continue;
        COutput out(&wallet->mapWallet[outpoint.hash], outpoint.n, nDepth, true, true);

        if(nCoinType == ALL_COINS){
            // We are now taking ALL_COINS to mean everything sans mints
            if(out.tx->tx->vout[out.i].scriptPubKey.IsZerocoinMint() || out.tx->tx->vout[out.i].scriptPubKey.IsSigmaMint() || out.tx->tx->vout[out.i].scriptPubKey.IsZerocoinRemint())
                continue;
        } else if(nCoinType == ONLY_MINTS){
            // Do not consider anything other than mints
            if(!(out.tx->tx->vout[out.i].scriptPubKey.IsZerocoinMint() || out.tx->tx->vout[out.i].scriptPubKey.IsSigmaMint() || out.tx->tx->vout[out.i].scriptPubKey.IsZerocoinRemint()))
                continue;
        }

        if (outpoint.n < out.tx->tx->vout.size() && wallet->IsMine(out.tx->tx->vout[outpoint.n]) == ISMINE_SPENDABLE)
            vCoins.push_back(out);
    }

    BOOST_FOREACH(const COutput& out, vCoins)
    {
        COutput cout = out;

        while (wallet->IsChange(cout.tx->GetHash(), cout.tx->tx->vout[cout.i]) && cout.tx->tx->vin.size() > 0 && wallet->IsMine(cout.tx->tx->vin[0]))
        {
            if (!wallet->mapWallet.count(cout.tx->tx->vin[0].prevout.hash)) break;
            cout = COutput(&wallet->mapWallet[cout.tx->tx->vin[0].prevout.hash], cout.tx->tx->vin[0].prevout.n, 0, true, true);
        }

        CTxDestination address;
        if(cout.tx->tx->IsZerocoinMint() || cout.tx->tx->IsSigmaMint() || cout.tx->tx->IsZerocoinRemint()){
            mapCoins[QString::fromStdString("(mint)")].push_back(out);
            continue;
        }
        else if(!out.fSpendable || !ExtractDestination(cout.tx->tx->vout[cout.i].scriptPubKey, address)){
            continue;
        }

        mapCoins[QString::fromStdString(CBitcoinAddress(address).ToString())].push_back(out);
    }
}

bool WalletModel::isLockedCoin(uint256 hash, unsigned int n) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->IsLockedCoin(hash, n);
}

void WalletModel::lockCoin(COutPoint& output)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->LockCoin(output);
    Q_EMIT updateMintable();
}

void WalletModel::unlockCoin(COutPoint& output)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->UnlockCoin(output);
    Q_EMIT updateMintable();
}

void WalletModel::listLockedCoins(std::vector<COutPoint>& vOutpts)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->ListLockedCoins(vOutpts);
}

void WalletModel::listProTxCoins(std::vector<COutPoint>& vOutpts)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->ListProTxCoins(vOutpts);
}

bool WalletModel::hasMasternode()
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->HasMasternode();
}

void WalletModel::loadReceiveRequests(std::vector<std::string>& vReceiveRequests)
{
    LOCK(wallet->cs_wallet);
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, CAddressBookData)& item, wallet->mapAddressBook)
        BOOST_FOREACH(const PAIRTYPE(std::string, std::string)& item2, item.second.destdata)
            if (item2.first.size() > 2 && item2.first.substr(0,2) == "rr") // receive request
                vReceiveRequests.push_back(item2.second);
}

void WalletModel::loadPCodeNotificationTransactions(std::vector<std::string>& vPCodeNotificationTransactions)
{
    LOCK(wallet->cs_wallet);
    wallet->loadPCodeNotificationTransactions(vPCodeNotificationTransactions);
    
}

bool WalletModel::saveReceiveRequest(const std::string &sAddress, const int64_t nId, const std::string &sRequest)
{
    CTxDestination dest = CBitcoinAddress(sAddress).Get();

    std::stringstream ss;
    ss << nId;
    std::string key = "rr" + ss.str(); // "rr" prefix = "receive request" in destdata

    LOCK(wallet->cs_wallet);
    if (sRequest.empty())
        return wallet->EraseDestData(dest, key);
    else
        return wallet->AddDestData(dest, key, sRequest);
    
}

bool WalletModel::savePCodeNotificationTransaction(const std::string &rpcodestr, const int64_t nId, const std::string &sNotificationSent)
{

    std::stringstream ss;
    ss << nId;
    std::string key = "pnts" + ss.str(); // "pnts" prefix = "paymentcode Notification transaction sent" in destdata

    LOCK(wallet->cs_wallet);
    if (sNotificationSent.empty())
        return wallet->ErasePCodeNotificationData(rpcodestr, key);
    else
        return wallet->AddPCodeNotificationData(rpcodestr, key, sNotificationSent);
    return true;
}

bool WalletModel::transactionCanBeAbandoned(uint256 hash) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    const CWalletTx *wtx = wallet->GetWalletTx(hash);
    if (!wtx || wtx->isAbandoned() || wtx->GetDepthInMainChain() > 0 || wtx->InMempool() || wtx->InStempool())
        return false;
    return true;
}

bool WalletModel::abandonTransaction(uint256 hash) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->AbandonTransaction(hash);
}

bool WalletModel::transactionCanBeRebroadcast(uint256 hash) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    const CWalletTx *wtx = wallet->GetWalletTx(hash);
    if (!wtx || wtx->isAbandoned() || wtx->GetDepthInMainChain() > 0)
        return false;
    return wtx->GetRequestCount() <= 0;
}

bool WalletModel::rebroadcastTransaction(uint256 hash)
{
    LOCK2(cs_main, wallet->cs_wallet);
    CWalletTx *wtx = const_cast<CWalletTx*>(wallet->GetWalletTx(hash));

    if (!wtx || wtx->isAbandoned() || wtx->GetDepthInMainChain() > 0)
        return false;
    if (wtx->GetRequestCount() > 0)
        return false;

    CCoinsViewCache &view = *pcoinsTip;
    bool fHaveChain = false;
    for (size_t i=0; i<wtx->tx->vout.size() && !fHaveChain; i++) {
        if (view.HaveCoin(COutPoint(hash, i)))
            fHaveChain = true;
    }

    bool fHaveMempool = mempool.exists(hash);

    if (!fHaveMempool && !fHaveChain) {
        // push to local node and sync with wallets
        CValidationState state;
        bool fMissingInputs;
        if (!AcceptToMemoryPool(mempool, state, wtx->tx, false, &fMissingInputs, NULL, true, false, maxTxFee))
            return false;
    } else if (fHaveChain) {
        return false;
    }

    g_connman->RelayTransaction(*wtx->tx);
    return true;
}

// Sigma
WalletModel::SendCoinsReturn WalletModel::prepareSigmaSpendTransaction(
    WalletModelTransaction &transaction,
    std::vector<CSigmaEntry> &selectedCoins,
    std::vector<CHDMint> &changes,
    bool& fChangeAddedToFee,
    const CCoinControl *coinControl)
{
    QList<SendCoinsRecipient> recipients = transaction.getRecipients();
    std::vector<CRecipient> sendRecipients;

    if (recipients.empty()) {
        return OK;
    }

    QSet<QString> addresses; // Used to detect duplicates

    for (const auto& rcp : recipients) {
        if (!validateAddress(rcp.address)) {
            return InvalidAmount;
        }
        addresses.insert(rcp.address);

        CScript scriptPubKey = GetScriptForDestination(CBitcoinAddress(rcp.address.toStdString()).Get());
        CRecipient recipient = {scriptPubKey, rcp.amount, rcp.fSubtractFeeFromAmount};
        sendRecipients.push_back(recipient);
    }

    if (addresses.size() != recipients.size()) {
        return DuplicateAddress;
    }

    // create transaction
    CAmount fee;

    CWalletTx *newTx = transaction.getTransaction();
    try {
        *newTx = wallet->CreateSigmaSpendTransaction(sendRecipients, fee, selectedCoins, changes, fChangeAddedToFee, coinControl);
    } catch (const InsufficientFunds& err) {
        return AmountExceedsBalance;
    } catch (const std::runtime_error& err) {
        if (_("Can not choose coins within limit.") == err.what())
            return ExceedLimit;
        throw err;
    } catch (const std::invalid_argument& err) {
        return ExceedLimit;
    }

    transaction.setTransactionFee(fee);

    return SendCoinsReturn(OK);
}

WalletModel::SendCoinsReturn WalletModel::prepareSigmaSpendPCodeTransaction(
    WalletModelTransaction &transaction,
    std::vector<CSigmaEntry> &selectedCoins,
    std::vector<CHDMint> &changes,
    bool& fChangeAddedToFee,
    const CCoinControl *coinControl)
{
    QList<SendCoinsRecipient> recipients = transaction.getRecipients();
    std::vector<CRecipient> sendRecipients;

    if (recipients.empty()) {
        return OK;
    }

    QSet<QString> addresses; // Used to detect duplicates

    for (const auto& rcp : recipients) {
        if(!validatePaymentCode(rcp.address))
        {
            return InvalidAddress;
        }
        if(rcp.amount <= 0)
        {
            return InvalidAmount;
        }
        
        
        
        // Get Or Create Payment Channel from payment code lgtm [cpp/commented-out-code] ;
        CBIP47PaymentChannel* channel = wallet->getPaymentChannelFromPaymentCode(rcp.address.toStdString());
        
        // If channel already sent notifcation transaction.
        if (channel->isNotificationTransactionSent())
        {
            std::string addressTo = wallet->getCurrentOutgoingAddress(*channel);
            addresses.insert(rcp.address);
            CBitcoinAddress pcAddress(addressTo);
            
            CScript scriptPubKey = GetScriptForDestination(pcAddress.Get());
            CRecipient recipient = {scriptPubKey, rcp.amount, rcp.fSubtractFeeFromAmount};
            sendRecipients.push_back(recipient);
            
        }
        else
        {
            return preparePCodeTransaction(transaction, coinControl);
        }
    }

    if (addresses.size() != recipients.size()) {
        return DuplicateAddress;
    }

    // create transaction
    CAmount fee;

    CWalletTx *newTx = transaction.getTransaction();
    try {
        *newTx = wallet->CreateSigmaSpendTransaction(sendRecipients, fee, selectedCoins, changes, fChangeAddedToFee, coinControl);
    } catch (const InsufficientFunds& err) {
        return AmountExceedsBalance;
    } catch (const std::runtime_error& err) {
        if (_("Can not choose coins within limit.") == err.what())
            return ExceedLimit;
        throw err;
    } catch (const std::invalid_argument& err) {
        return ExceedLimit;
    }

    transaction.setTransactionFee(fee);

    return SendCoinsReturn(OK);
}



WalletModel::SendCoinsReturn WalletModel::sendSigma(WalletModelTransaction &transaction,
    std::vector<CSigmaEntry>& coins, std::vector<CHDMint>& changes)
{
    QByteArray transaction_array; /* store serialized transaction */

    {
        LOCK2(cs_main, wallet->cs_wallet);
        CWalletTx *newTx = transaction.getTransaction();

        for (const auto& rcp : transaction.getRecipients()) {
            if (rcp.paymentRequest.IsInitialized())
            {
                // Make sure any payment requests involved are still valid.
                if (PaymentServer::verifyExpired(rcp.paymentRequest.getDetails())) {
                    return PaymentRequestExpired;
                }

                // Store PaymentRequests in wtx.vOrderForm in wallet.
                std::string key("PaymentRequest");
                std::string value;
                rcp.paymentRequest.SerializeToString(&value);
                newTx->vOrderForm.push_back(std::make_pair(key, value));
            } else if (!rcp.message.isEmpty()) {
                // Message from normal zcoin:URI (zcoin:123...?message=example)
                newTx->vOrderForm.push_back(std::make_pair("Message", rcp.message.toStdString()));
            }
        }

        try {
            wallet->CommitSigmaTransaction(*newTx, coins, changes);
        } catch (...) {
            return TransactionCommitFailed;
        }

        CTransactionRef t = newTx->tx;
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << t;
        transaction_array.append(&(ssTx[0]), ssTx.size());
    }

    // Add addresses / update labels that we've sent to to the address book,
    // and emit coinsSent signal for each recipient
    for (const auto& rcp : transaction.getRecipients()) {
        // Don't touch the address book when we have a payment request
        if (!rcp.paymentRequest.IsInitialized()) {
            std::string address = rcp.address.toStdString();
            CTxDestination dest = CBitcoinAddress(address).Get();
            std::string label = rcp.label.toStdString();
            {
                LOCK(wallet->cs_wallet);

                auto mi = wallet->mapAddressBook.find(dest);

                // Check if we have a new address or an updated label
                if (mi == wallet->mapAddressBook.end()) {
                    wallet->SetAddressBook(dest, label, "send");
                }
                else if (mi->second.name != label) {
                    wallet->SetAddressBook(dest, label, ""); // "" means don't change purpose
                }
            }
        }
        Q_EMIT coinsSent(wallet, rcp, transaction_array);
    }
    checkBalanceChanged();

    return SendCoinsReturn(OK);
}

WalletModel::SendCoinsReturn WalletModel::sendSigmaPCode(WalletModelTransaction &transaction,
    std::vector<CSigmaEntry>& coins, std::vector<CHDMint>& changes)
{
    QByteArray transaction_array; /* store serialized transaction */

    {
        LOCK2(cs_main, wallet->cs_wallet);
        CWalletTx *newTx = transaction.getTransaction();

        for (const auto& rcp : transaction.getRecipients()) {
            if (!rcp.message.isEmpty()) {
                // Message from normal zcoin:URI (zcoin:123...?message=example)
                newTx->vOrderForm.push_back(std::make_pair("Message", rcp.message.toStdString()));
            }
        }

        try {
            wallet->CommitSigmaTransaction(*newTx, coins, changes);
        } catch (...) {
            return TransactionCommitFailed;
        }
        
        const CTransaction* t = (newTx->tx.get());
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << *t;
        transaction_array.append(&(ssTx[0]), ssTx.size());
    }

    // Add addresses / update labels that we've sent to to the address book,
    // and emit coinsSent signal for each recipient
    for (const auto& rcp : transaction.getRecipients()) {
        // Don't touch the address book when we have a payment request
        std::string pcodestr = rcp.address.toStdString();
        std::string strLabel = rcp.label.toStdString();
        CBIP47PaymentChannel pchannel(pcodestr, strLabel);

        // Add to payment channel return true or false lgtm [cpp/commented-out-code] ;
        wallet->addToCBIP47PaymentChannel(pchannel);
        CBIP47PaymentChannel* channel = wallet->getPaymentChannelFromPaymentCode(pcodestr);
        channel->setLabel(strLabel);
        if(!channel->isNotificationTransactionSent())
        {
            channel->setStatusSent(transaction.getTransaction()->GetHash());
        }
        else 
        {
            std::string pcoutaddress = wallet->getCurrentOutgoingAddress(*channel);
            channel->addAddressToOutgoingAddresses(pcoutaddress);
            channel->incrementOutgoingIndex();
            
        }
        wallet->saveCBIP47PaymentChannelData(pcodestr);
        paymentCodeTableModel->refreshModel();
        Q_EMIT coinsSent(wallet, rcp, transaction_array);
    }
    checkBalanceChanged();

    return SendCoinsReturn(OK);
}

void WalletModel::sigmaMint(const CAmount& n, const CCoinControl *coinControl)
{
    std::vector<sigma::CoinDenomination> denominations;
    sigma::GetAllDenoms(denominations);

    std::vector<sigma::CoinDenomination> mints;
    if (CWallet::SelectMintCoinsForAmount(n, denominations, mints) != n) {
        throw std::runtime_error("Problem with coin selection.\n");
    }

    std::vector<sigma::PrivateCoin> privCoins;

    const sigma::Params* sigmaParams = sigma::Params::get_default();
    std::transform(mints.begin(), mints.end(), std::back_inserter(privCoins),
        [sigmaParams](const sigma::CoinDenomination& denom) -> sigma::PrivateCoin {
            return sigma::PrivateCoin(sigmaParams, denom);
        });

    vector<CHDMint> vDMints;
    auto recipients = CWallet::CreateSigmaMintRecipients(privCoins, vDMints);

    CWalletTx wtx;
    std::string strError = pwalletMain->MintAndStoreSigma(recipients, privCoins, vDMints, wtx, false, coinControl);

    if (strError != "") {
        throw std::range_error(strError);
    }
}

std::vector<CSigmaEntry> WalletModel::GetUnsafeCoins(const CCoinControl* coinControl)
{
    auto allCoins = wallet->GetAvailableCoins(coinControl, true);
    auto spendableCoins = wallet->GetAvailableCoins(coinControl);
    std::vector<CSigmaEntry> unsafeCoins;
    for (auto& coin : allCoins) {
        if (spendableCoins.end() == std::find_if(spendableCoins.begin(), spendableCoins.end(),
            [coin](const CSigmaEntry& spendalbe) {
                return coin.value == spendalbe.value;
            }
        )) {
            unsafeCoins.push_back(coin);
        }
    }
    return unsafeCoins;
}

bool WalletModel::isWalletEnabled()
{
   return !GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET);
}

bool WalletModel::hdEnabled() const
{
    return wallet->IsHDEnabled();
}

int WalletModel::getDefaultConfirmTarget() const
{
    return nTxConfirmTarget;
}
