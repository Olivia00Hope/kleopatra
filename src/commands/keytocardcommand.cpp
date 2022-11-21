/* commands/keytocardcommand.cpp

    This file is part of Kleopatra, the KDE keymanager
    SPDX-FileCopyrightText: 2017 Bundesamt für Sicherheit in der Informationstechnik
    SPDX-FileContributor: Intevation GmbH
    SPDX-FileCopyrightText: 2020 g10 Code GmbH
    SPDX-FileContributor: Ingo Klöcker <dev@ingo-kloecker.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <config-kleopatra.h>

#include "keytocardcommand.h"

#include "cardcommand_p.h"

#include "commands/authenticatepivcardapplicationcommand.h"

#include "smartcard/openpgpcard.h"
#include "smartcard/pivcard.h"
#include "smartcard/readerstatus.h"
#include "smartcard/utils.h"

#include <Libkleo/Dn>
#include <Libkleo/Formatting>
#include <Libkleo/KeyCache>
#include <Libkleo/KeySelectionDialog>

#include <KLocalizedString>

#include <QDateTime>
#include <QInputDialog>
#include <QStringList>

#include <gpg-error.h>
#if GPG_ERROR_VERSION_NUMBER >= 0x12400 // 1.36
# define GPG_ERROR_HAS_NO_AUTH
#endif

#include "kleopatra_debug.h"

using namespace Kleo;
using namespace Kleo::Commands;
using namespace Kleo::SmartCard;
using namespace GpgME;

class KeyToCardCommand::Private : public CardCommand::Private
{
    friend class ::Kleo::Commands::KeyToCardCommand;
    KeyToCardCommand *q_func() const
    {
        return static_cast<KeyToCardCommand *>(q);
    }
public:
    explicit Private(KeyToCardCommand *qq, const GpgME::Subkey &subkey);
    explicit Private(KeyToCardCommand *qq, const std::string &slot, const std::string &serialNumber, const std::string &appName);
    ~Private() override;

private:
    void start();

    void startKeyToOpenPGPCard();

    Subkey getSubkeyToTransferToPIVCard(const std::string &cardSlot, const std::shared_ptr<PIVCard> &card);
    void startKeyToPIVCard();

    void authenticate();
    void authenticationFinished();
    void authenticationCanceled();

    void keyToCardDone(const GpgME::Error &err);
    void keyToPIVCardDone(const GpgME::Error &err);

    void keyHasBeenCopiedToCard();

private:
    std::string appName;
    GpgME::Subkey subkey;
    std::string cardSlot;
    bool overwriteExistingAlreadyApproved = false;
    bool hasBeenCanceled = false;
};

KeyToCardCommand::Private *KeyToCardCommand::d_func()
{
    return static_cast<Private *>(d.get());
}
const KeyToCardCommand::Private *KeyToCardCommand::d_func() const
{
    return static_cast<const Private *>(d.get());
}

#define q q_func()
#define d d_func()


KeyToCardCommand::Private::Private(KeyToCardCommand *qq, const GpgME::Subkey &subkey_)
    : CardCommand::Private(qq, "", nullptr)
    , subkey(subkey_)
{
}

KeyToCardCommand::Private::Private(KeyToCardCommand *qq, const std::string &slot, const std::string &serialNumber, const std::string &appName_)
    : CardCommand::Private(qq, serialNumber, nullptr)
    , appName(appName_)
    , cardSlot(slot)
{
}

KeyToCardCommand::Private::~Private()
{
}

namespace {
static std::shared_ptr<Card> getCardToTransferSubkeyTo(const Subkey &subkey, QWidget *parent)
{
    const std::vector<std::shared_ptr<Card> > suitableCards = KeyToCardCommand::getSuitableCards(subkey);
    if (suitableCards.empty()) {
        return std::shared_ptr<Card>();
    } else if (suitableCards.size() == 1) {
        return suitableCards[0];
    }

    QStringList options;
    for (const auto &card: suitableCards) {
        options.push_back(i18nc("smartcard application - serial number of smartcard", "%1 - %2",
            displayAppName(card->appName()), card->displaySerialNumber()));
    }

    bool ok;
    const QString choice = QInputDialog::getItem(parent, i18n("Select Card"),
        i18n("Please select the card the key should be written to:"), options, /* current= */ 0, /* editable= */ false, &ok);
    if (!ok) {
        return std::shared_ptr<Card>();
    }
    const int index = options.indexOf(choice);
    return suitableCards[index];
}
}

void KeyToCardCommand::Private::start()
{
    qCDebug(KLEOPATRA_LOG) << "KeyToCardCommand::Private::start()";

    if (!subkey.isNull() && serialNumber().empty()) {
        const auto card = getCardToTransferSubkeyTo(subkey, parentWidgetOrView());
        if (!card) {
            finished();
            return;
        }
        setSerialNumber(card->serialNumber());
        appName = card->appName();
    }

    const auto card = SmartCard::ReaderStatus::instance()->getCard(serialNumber(), appName);
    if (!card) {
        error(i18n("Failed to find the card with the serial number: %1", QString::fromStdString(serialNumber())));
        finished();
        return;
    }

    if (card->appName() == SmartCard::OpenPGPCard::AppName) {
        startKeyToOpenPGPCard();
    } else if (card->appName() == SmartCard::PIVCard::AppName) {
        startKeyToPIVCard();
    } else {
        error(i18n("Sorry! Transferring keys to this card is not supported."));
        finished();
        return;
    }
}

namespace {
static int getOpenPGPCardSlotForKey(const GpgME::Subkey &subKey, QWidget *parent)
{
    // Check if we need to ask the user for the slot
    if ((subKey.canSign() || subKey.canCertify()) && !subKey.canEncrypt() && !subKey.canAuthenticate()) {
        // Signing only
        return 1;
    }
    if (subKey.canEncrypt() && !(subKey.canSign() || subKey.canCertify()) && !subKey.canAuthenticate()) {
        // Encrypt only
        return 2;
    }
    if (subKey.canAuthenticate() && !(subKey.canSign() || subKey.canCertify()) && !subKey.canEncrypt()) {
        // Auth only
        return 3;
    }
    // Multiple uses, ask user.
    QStringList options;

    if (subKey.canSign() || subKey.canCertify()) {
        options << i18nc("Placeholder is the number of a slot on a smart card", "Signature (%1)", 1);
    }
    if (subKey.canEncrypt()) {
        options << i18nc("Placeholder is the number of a slot on a smart card", "Encryption (%1)", 2);
    }
    if (subKey.canAuthenticate()) {
        options << i18nc("Placeholder is the number of a slot on a smart card", "Authentication (%1)", 3);
    }

    bool ok;
    const QString choice = QInputDialog::getItem(parent, i18n("Select Card Slot"),
        i18n("Please select the card slot the key should be written to:"), options, /* current= */ 0, /* editable= */ false, &ok);
    const int slot = options.indexOf(choice) + 1;
    return ok ? slot : -1;
}
}

void KeyToCardCommand::Private::startKeyToOpenPGPCard() {
    qCDebug(KLEOPATRA_LOG) << "KeyToCardCommand::Private::startKeyToOpenPGPCard()";

    const auto pgpCard = SmartCard::ReaderStatus::instance()->getCard<OpenPGPCard>(serialNumber());
    if (!pgpCard) {
        error(i18n("Failed to find the OpenPGP card with the serial number: %1", QString::fromStdString(serialNumber())));
        finished();
        return;
    }

    if (subkey.isNull()) {
        finished();
        return;
    }
    if (subkey.parent().protocol() != GpgME::OpenPGP) {
        error(i18n("Sorry! This key cannot be transferred to an OpenPGP card."));
        finished();
        return;
    }

    const auto slot = getOpenPGPCardSlotForKey(subkey, parentWidgetOrView());
    if (slot < 1) {
        finished();
        return;
    }

    // Check if we need to do the overwrite warning.
    std::string existingKey;
    QString encKeyWarning;
    if (slot == 1) {
        existingKey = pgpCard->keyFingerprint(OpenPGPCard::pgpSigKeyRef());
    } else if (slot == 2) {
        existingKey = pgpCard->keyFingerprint(OpenPGPCard::pgpEncKeyRef());
        encKeyWarning = i18n("It will no longer be possible to decrypt past communication "
                                "encrypted for the existing key.");
    } else if (slot == 3) {
        existingKey = pgpCard->keyFingerprint(OpenPGPCard::pgpAuthKeyRef());
    }
    if (!existingKey.empty()) {
        const QString message = i18nc("@info",
            "<p>This card already contains a key in this slot. Continuing will <b>overwrite</b> that key.</p>"
            "<p>If there is no backup the existing key will be irrecoverably lost.</p>") +
            i18n("The existing key has the fingerprint:") +
            QStringLiteral("<pre>%1</pre>").arg(QString::fromStdString(existingKey)) +
            encKeyWarning;
        const auto choice = KMessageBox::warningContinueCancel(parentWidgetOrView(), message,
            i18nc("@title:window", "Overwrite existing key"),
            KStandardGuiItem::cont(), KStandardGuiItem::cancel(), QString(), KMessageBox::Notify | KMessageBox::Dangerous);
        if (choice != KMessageBox::Continue) {
            finished();
            return;
        }
    }

    // Now do the deed
    const auto time = QDateTime::fromSecsSinceEpoch(quint32(subkey.creationTime()), Qt::UTC);
    const auto timestamp = time.toString(QStringLiteral("yyyyMMdd'T'HHmmss"));
    const QString cmd = QStringLiteral("KEYTOCARD --force %1 %2 OPENPGP.%3 %4")
        .arg(QString::fromLatin1(subkey.keyGrip()), QString::fromStdString(serialNumber()))
        .arg(slot)
        .arg(timestamp);
    ReaderStatus::mutableInstance()->startSimpleTransaction(pgpCard, cmd.toUtf8(), q_func(), [this](const GpgME::Error &err) {
        keyToCardDone(err);
    });
}

namespace {
static std::vector<Key> getSigningCertificates()
{
    std::vector<Key> signingCertificates = KeyCache::instance()->secretKeys();
    const auto it = std::remove_if(signingCertificates.begin(), signingCertificates.end(),
                                   [](const Key &key) {
                                       return ! (key.protocol() == GpgME::CMS &&
                                                 !key.subkey(0).isNull() &&
                                                 key.subkey(0).canSign() &&
                                                 !key.subkey(0).canEncrypt() &&
                                                 key.subkey(0).isSecret() &&
                                                 !key.subkey(0).isCardKey());
                                   });
    signingCertificates.erase(it, signingCertificates.end());
    return signingCertificates;
}

static std::vector<Key> getEncryptionCertificates()
{
    std::vector<Key> encryptionCertificates = KeyCache::instance()->secretKeys();
    const auto it = std::remove_if(encryptionCertificates.begin(), encryptionCertificates.end(),
                                   [](const Key &key) {
                                       return ! (key.protocol() == GpgME::CMS &&
                                                 !key.subkey(0).isNull() &&
                                                 key.subkey(0).canEncrypt() &&
                                                 key.subkey(0).isSecret() &&
                                                 !key.subkey(0).isCardKey());
                                   });
    encryptionCertificates.erase(it, encryptionCertificates.end());
    return encryptionCertificates;
}
}

Subkey KeyToCardCommand::Private::getSubkeyToTransferToPIVCard(const std::string &cardSlot, const std::shared_ptr<PIVCard> &/*card*/)
{
    if (cardSlot != PIVCard::cardAuthenticationKeyRef() && cardSlot != PIVCard::keyManagementKeyRef()) {
        return Subkey();
    }

    const std::vector<Key> certificates = cardSlot == PIVCard::cardAuthenticationKeyRef() ? getSigningCertificates() : getEncryptionCertificates();
    if (certificates.empty()) {
        error(i18n("Sorry! No suitable certificate to write to this card slot was found."));
        return Subkey();
    }

    auto dialog = new KeySelectionDialog(parentWidgetOrView());
    dialog->setWindowTitle(i18nc("@title:window", "Select Certificate"));
    dialog->setText(i18n("Please select the certificate whose key pair you want to write to the card:"));
    dialog->setKeys(certificates);

    if (dialog->exec() == QDialog::Rejected) {
        return Subkey();
    }

    return dialog->selectedKey().subkey(0);
}

void KeyToCardCommand::Private::startKeyToPIVCard()
{
    qCDebug(KLEOPATRA_LOG) << "KeyToCardCommand::Private::startKeyToPIVCard()";

    const auto pivCard = SmartCard::ReaderStatus::instance()->getCard<PIVCard>(serialNumber());
    if (!pivCard) {
        error(i18n("Failed to find the PIV card with the serial number: %1", QString::fromStdString(serialNumber())));
        finished();
        return;
    }

    if (cardSlot != PIVCard::cardAuthenticationKeyRef() && cardSlot != PIVCard::keyManagementKeyRef()) {
        // key to card is only supported for the Card Authentication key and the Key Management key
        finished();
        return;
    }

    if (subkey.isNull()) {
        subkey = getSubkeyToTransferToPIVCard(cardSlot, pivCard);
    }
    if (subkey.isNull()) {
        finished();
        return;
    }
    if (subkey.parent().protocol() != GpgME::CMS) {
        error(i18n("Sorry! This key cannot be transferred to a PIV card."));
        finished();
        return;
    }
    if (!subkey.canEncrypt() && !subkey.canSign()) {
        error(i18n("Sorry! Only encryption keys and signing keys can be transferred to a PIV card."));
        finished();
        return;
    }

    // Check if we need to do the overwrite warning.
    if (!overwriteExistingAlreadyApproved) {
        const std::string existingKey = pivCard->keyInfo(cardSlot).grip;
        if (!existingKey.empty() && (existingKey != subkey.keyGrip())) {
            const QString decryptionWarning = (cardSlot == PIVCard::keyManagementKeyRef()) ?
                i18n("It will no longer be possible to decrypt past communication encrypted for the existing key.") :
                QString();
            const QString message = i18nc("@info",
                "<p>This card already contains a key in this slot. Continuing will <b>overwrite</b> that key.</p>"
                "<p>If there is no backup the existing key will be irrecoverably lost.</p>") +
                i18n("The existing key has the key grip:") +
                QStringLiteral("<pre>%1</pre>").arg(QString::fromStdString(existingKey)) +
                decryptionWarning;
            const auto choice = KMessageBox::warningContinueCancel(parentWidgetOrView(), message,
                i18nc("@title:window", "Overwrite existing key"),
                KStandardGuiItem::cont(), KStandardGuiItem::cancel(), QString(), KMessageBox::Notify | KMessageBox::Dangerous);
            if (choice != KMessageBox::Continue) {
                finished();
                return;
            }
            overwriteExistingAlreadyApproved = true;
        }
    }

    const QString cmd = QStringLiteral("KEYTOCARD --force %1 %2 %3")
        .arg(QString::fromLatin1(subkey.keyGrip()), QString::fromStdString(serialNumber()))
        .arg(QString::fromStdString(cardSlot));
    ReaderStatus::mutableInstance()->startSimpleTransaction(pivCard, cmd.toUtf8(), q_func(), [this](const GpgME::Error &err) {
        keyToPIVCardDone(err);
    });
}

void KeyToCardCommand::Private::authenticate()
{
    qCDebug(KLEOPATRA_LOG) << "KeyToCardCommand::authenticate()";

    auto cmd = new AuthenticatePIVCardApplicationCommand(serialNumber(), parentWidgetOrView());
    connect(cmd, &AuthenticatePIVCardApplicationCommand::finished,
            q, [this]() { authenticationFinished(); });
    connect(cmd, &AuthenticatePIVCardApplicationCommand::canceled,
            q, [this]() { authenticationCanceled(); });
    cmd->start();
}

void KeyToCardCommand::Private::authenticationFinished()
{
    qCDebug(KLEOPATRA_LOG) << "KeyToCardCommand::authenticationFinished()";
    if (!hasBeenCanceled) {
        startKeyToPIVCard();
    }
}

void KeyToCardCommand::Private::authenticationCanceled()
{
    qCDebug(KLEOPATRA_LOG) << "KeyToCardCommand::authenticationCanceled()";
    hasBeenCanceled = true;
    canceled();
}

void KeyToCardCommand::Private::keyHasBeenCopiedToCard()
{
    ReaderStatus::mutableInstance()->updateStatus();
    success(i18nc("@info", "Successfully copied the key to the card."));
    finished();
}

KeyToCardCommand::KeyToCardCommand(const GpgME::Subkey &subkey)
    : CardCommand(new Private(this, subkey))
{
}

KeyToCardCommand::KeyToCardCommand(const std::string& cardSlot, const std::string &serialNumber, const std::string &appName)
    : CardCommand(new Private(this, cardSlot, serialNumber, appName))
{
}

KeyToCardCommand::~KeyToCardCommand()
{
    qCDebug(KLEOPATRA_LOG) << "KeyToCardCommand::~KeyToCardCommand()";
}

// static
std::vector<std::shared_ptr<Card> > KeyToCardCommand::getSuitableCards(const GpgME::Subkey &subkey)
{
    std::vector<std::shared_ptr<Card> > suitableCards;
    if (subkey.isNull() || subkey.parent().protocol() != GpgME::OpenPGP) {
        return suitableCards;
    }
    for (const auto &card: ReaderStatus::instance()->getCards()) {
        if (card->appName() == OpenPGPCard::AppName) {
            suitableCards.push_back(card);
        }
    }
    return suitableCards;
}

void KeyToCardCommand::Private::keyToCardDone(const GpgME::Error &err)
{
    if (!err && !err.isCanceled()) {
        keyHasBeenCopiedToCard();
        return;
    }
    if (err) {
        error(xi18nc("@info",
                     "<para>Copying the key to the card failed:</para><para><message>%1</message></para>", QString::fromUtf8(err.asString())));
    }
    finished();
}

void KeyToCardCommand::Private::keyToPIVCardDone(const GpgME::Error &err)
{
    qCDebug(KLEOPATRA_LOG) << q << __func__ << err.asString() << "(" << err.code() << ")";
#ifdef GPG_ERROR_HAS_NO_AUTH
    // gpgme 1.13 reports "BAD PIN" instead of "NO AUTH"
    if (err.code() == GPG_ERR_NO_AUTH || err.code() == GPG_ERR_BAD_PIN) {
        authenticate();
        return;
    }
#endif
    keyToCardDone(err);
}

void KeyToCardCommand::doStart()
{
    qCDebug(KLEOPATRA_LOG) << "KeyToCardCommand::doStart()";

    d->start();
}

void KeyToCardCommand::doCancel()
{
}

#undef q_func
#undef d_func
