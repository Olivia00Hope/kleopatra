/* -*- mode: c++; c-basic-offset:4 -*-
    commands/createopenpgpkeyfromcardkeyscommand.cpp

    This file is part of Kleopatra, the KDE keymanager
    SPDX-FileCopyrightText: 2020 g10 Code GmbH
    SPDX-FileContributor: Ingo Klöcker <dev@ingo-kloecker.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <config-kleopatra.h>

#include "createopenpgpkeyfromcardkeyscommand.h"

#include "cardcommand_p.h"

#include "dialogs/adduseriddialog.h"

#include "smartcard/openpgpcard.h"
#include "smartcard/readerstatus.h"

#include <Libkleo/Formatting>
#include <Libkleo/KeyCache>

#include <KLocalizedString>

#include <QGpgME/Protocol>
#include <QGpgME/QuickJob>

#include <gpgme++/context.h>
#include <gpgme++/engineinfo.h>

#include <gpgme.h>

#include "kleopatra_debug.h"

using namespace Kleo;
using namespace Kleo::Commands;
using namespace Kleo::Dialogs;
using namespace Kleo::SmartCard;
using namespace GpgME;
using namespace QGpgME;

class CreateOpenPGPKeyFromCardKeysCommand::Private : public CardCommand::Private
{
    friend class ::Kleo::Commands::CreateOpenPGPKeyFromCardKeysCommand;
    CreateOpenPGPKeyFromCardKeysCommand *q_func() const
    {
        return static_cast<CreateOpenPGPKeyFromCardKeysCommand *>(q);
    }
public:
    explicit Private(CreateOpenPGPKeyFromCardKeysCommand *qq, const std::string &serialNumber, const std::string &appName, QWidget *parent);
    ~Private();

private:
    void start();

    void slotDialogAccepted();
    void slotDialogRejected();
    void slotResult(const Error &err);

    void ensureDialogCreated();

private:
    std::string appName;
    QPointer<AddUserIDDialog> dialog;
};

CreateOpenPGPKeyFromCardKeysCommand::Private *CreateOpenPGPKeyFromCardKeysCommand::d_func()
{
    return static_cast<Private *>(d.get());
}
const CreateOpenPGPKeyFromCardKeysCommand::Private *CreateOpenPGPKeyFromCardKeysCommand::d_func() const
{
    return static_cast<const Private *>(d.get());
}

#define d d_func()
#define q q_func()

CreateOpenPGPKeyFromCardKeysCommand::Private::Private(CreateOpenPGPKeyFromCardKeysCommand *qq, const std::string &serialNumber, const std::string &appName_, QWidget *parent)
    : CardCommand::Private(qq, serialNumber, parent)
    , appName(appName_)
{
}

CreateOpenPGPKeyFromCardKeysCommand::Private::~Private()
{
}

void CreateOpenPGPKeyFromCardKeysCommand::Private::start()
{
    const auto pgpCard = ReaderStatus::instance()->getCard<OpenPGPCard>(serialNumber());
    if (!pgpCard) {
        error(i18n("Failed to find the OpenPGP card with the serial number: %1", QString::fromStdString(serialNumber())));
        finished();
        return;
    }

    const auto signingKey = KeyCache::instance()->findByKeyIDOrFingerprint(pgpCard->sigFpr());
    if (!signingKey.isNull()) {
        const QString message = i18nc("@info",
            "<p>There is already an OpenPGP key corresponding to the signing key on this card:</p><p>%1</p>"
            "<p>Do you still want to create an OpenPGP key for the card keys?</p>",
            Formatting::summaryLine(signingKey));
        const auto choice = KMessageBox::warningContinueCancel(parentWidgetOrView(), message,
            i18nc("@title:window", "Create OpenPGP Key"),
            KStandardGuiItem::cont(), KStandardGuiItem::cancel(), QString(), KMessageBox::Notify);
        if (choice != KMessageBox::Continue) {
            finished();
            return;
        }
    }

    ensureDialogCreated();

    dialog->setWindowTitle(i18n("Enter User ID"));
    dialog->setName(pgpCard->cardHolder());

    dialog->show();
}

void CreateOpenPGPKeyFromCardKeysCommand::Private::slotDialogAccepted()
{
    if (!(engineInfo(GpgEngine).engineVersion() < "2.3.0")) {
        Error err;
        std::unique_ptr<Context> c = Context::createForEngine(AssuanEngine, &err);
        if (err.code() == GPG_ERR_NOT_SUPPORTED) {
            finished();
            return;
        }
        auto assuanContext = std::shared_ptr<Context>(c.release());
        const auto resultSerialNumber = ReaderStatus::switchCard(assuanContext, serialNumber(), err);
        if (err || resultSerialNumber != serialNumber()) {
            qCWarning(KLEOPATRA_LOG) << "Switching to card" << QString::fromStdString(serialNumber()) << "failed";
            finished();
            return;
        }
        const auto resultAppName = ReaderStatus::switchApp(assuanContext, serialNumber(), appName, err);
        if (err || resultAppName != appName) {
            qCWarning(KLEOPATRA_LOG) << "Switching card to" << QString::fromStdString(appName) << "app failed";
            finished();
            return;
        }
    }

    const auto backend = openpgp();
    if (!backend) {
        finished();
        return;
    }

    QuickJob *const job = backend->quickJob();
    if (!job) {
        finished();
        return;
    }

    connect(job, SIGNAL(result(GpgME::Error)),
            q, SLOT(slotResult(GpgME::Error)));

    const QString userID = Formatting::prettyNameAndEMail(OpenPGP, QString(), dialog->name(), dialog->email(), dialog->comment());
    const QDateTime expires = QDateTime();
    const unsigned int flags = GPGME_CREATE_FORCE;
    job->startCreate(userID, "card", expires, Key(), flags);
}

void CreateOpenPGPKeyFromCardKeysCommand::Private::slotDialogRejected()
{
    canceled();
}

void CreateOpenPGPKeyFromCardKeysCommand::Private::slotResult(const Error &err)
{
    if (err.isCanceled()) {
        // do nothing
    } else if (err) {
        error(i18nc("@info", "Creating an OpenPGP key from the card keys failed: %1", QString::fromUtf8(err.asString())),
              i18nc("@title", "Error"));
    } else {
        information(i18nc("@info", "Successfully generated an OpenPGP key from the card keys."),
                    i18nc("@title", "Success"));
    }

    finished();
}

void CreateOpenPGPKeyFromCardKeysCommand::Private::ensureDialogCreated()
{
    if (dialog) {
        return;
    }

    dialog = new AddUserIDDialog;
    applyWindowID(dialog);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    connect(dialog, SIGNAL(accepted()), q, SLOT(slotDialogAccepted()));
    connect(dialog, SIGNAL(rejected()), q, SLOT(slotDialogRejected()));
}

CreateOpenPGPKeyFromCardKeysCommand::CreateOpenPGPKeyFromCardKeysCommand(const std::string &serialNumber, const std::string &appName, QWidget *parent)
    : CardCommand(new Private(this, serialNumber, appName, parent))
{
}

CreateOpenPGPKeyFromCardKeysCommand::~CreateOpenPGPKeyFromCardKeysCommand()
{
}

void CreateOpenPGPKeyFromCardKeysCommand::doStart()
{
    d->start();
}

void CreateOpenPGPKeyFromCardKeysCommand::doCancel()
{
}

#undef d
#undef q

#include "moc_createopenpgpkeyfromcardkeyscommand.cpp"
