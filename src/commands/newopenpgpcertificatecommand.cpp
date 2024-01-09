/* -*- mode: c++; c-basic-offset:4 -*-
    commands/newopenpgpcertificatecommand.cpp

    This file is part of Kleopatra, the KDE keymanager
    SPDX-FileCopyrightText: 2008 Klarälvdalens Datakonsult AB
    SPDX-FileCopyrightText: 2022 g10 Code GmbH
    SPDX-FileContributor: Ingo Klöcker <dev@ingo-kloecker.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <config-kleopatra.h>

#include "newopenpgpcertificatecommand.h"

#include "command_p.h"

#include "dialogs/newopenpgpcertificatedetailsdialog.h"
#include "kleopatraapplication.h"
#include "utils/emptypassphraseprovider.h"
#include "utils/keyparameters.h"
#include "utils/userinfo.h"

#include <settings.h>

#include <Libkleo/Formatting>
#include <Libkleo/KeyCache>

#include <KConfigGroup>
#include <KLocalizedString>
#include <KMessageBox>
#include <KSharedConfig>

#include <QGpgME/KeyGenerationJob>
#include <QGpgME/Protocol>
#include <QGpgME/QuickJob>

#include <QProgressDialog>
#include <QSettings>

#include <gpgme++/context.h>
#include <gpgme++/keygenerationresult.h>

#include <kleopatra_debug.h>

using namespace Kleo;
using namespace GpgME;

class NewOpenPGPCertificateCommand::Private : public Command::Private
{
    friend class ::Kleo::NewOpenPGPCertificateCommand;
    NewOpenPGPCertificateCommand *q_func() const
    {
        return static_cast<NewOpenPGPCertificateCommand *>(q);
    }

public:
    explicit Private(NewOpenPGPCertificateCommand *qq, KeyListController *c)
        : Command::Private{qq, c}
    {
    }

    void getCertificateDetails();
    void createCertificate();
    void showResult(const KeyGenerationResult &result, const Key &key, const GpgME::Error &error);
    void showErrorDialog(const KeyGenerationResult &result);
    void addAdsk(const Key &key, const QString &fingerprint, const KeyGenerationResult &result);
    void handleKeyGenerationResult(const KeyGenerationResult &result);

private:
    KeyParameters keyParameters;
    bool protectKeyWithPassword = false;
    EmptyPassphraseProvider emptyPassphraseProvider;
    QPointer<NewOpenPGPCertificateDetailsDialog> detailsDialog;
    QPointer<QGpgME::Job> job;
    QPointer<QProgressDialog> progressDialog;
    QString adskfpr;
};

NewOpenPGPCertificateCommand::Private *NewOpenPGPCertificateCommand::d_func()
{
    return static_cast<Private *>(d.get());
}
const NewOpenPGPCertificateCommand::Private *NewOpenPGPCertificateCommand::d_func() const
{
    return static_cast<const Private *>(d.get());
}

#define d d_func()
#define q q_func()

void NewOpenPGPCertificateCommand::Private::getCertificateDetails()
{
    detailsDialog = new NewOpenPGPCertificateDetailsDialog;
    detailsDialog->setAttribute(Qt::WA_DeleteOnClose);
    applyWindowID(detailsDialog);

    if (keyParameters.protocol() == KeyParameters::NoProtocol) {
        const auto settings = Kleo::Settings{};
        const KConfigGroup config{KSharedConfig::openConfig(), QLatin1String("CertificateCreationWizard")};
        // prefer the last used name and email address over the values retrieved from the system
        detailsDialog->setName(config.readEntry("NAME", QString{}));
        if (detailsDialog->name().isEmpty() && settings.prefillName()) {
            detailsDialog->setName(userFullName());
        }
        detailsDialog->setEmail(config.readEntry("EMAIL", QString{}));
        if (detailsDialog->email().isEmpty() && settings.prefillEmail()) {
            detailsDialog->setEmail(userEmailAddress());
        }
    } else {
        detailsDialog->setKeyParameters(keyParameters);
        detailsDialog->setProtectKeyWithPassword(protectKeyWithPassword);
    }

    connect(detailsDialog, &QDialog::accepted, q, [this]() {
        keyParameters = detailsDialog->keyParameters();
        protectKeyWithPassword = detailsDialog->protectKeyWithPassword();
        QMetaObject::invokeMethod(
            q,
            [this] {
                createCertificate();
            },
            Qt::QueuedConnection);
    });
    connect(detailsDialog, &QDialog::rejected, q, [this]() {
        canceled();
    });

    detailsDialog->show();
}

void NewOpenPGPCertificateCommand::Private::createCertificate()
{
    Q_ASSERT(keyParameters.protocol() == KeyParameters::OpenPGP);

    auto keyGenJob = QGpgME::openpgp()->keyGenerationJob();
    if (!keyGenJob) {
        finished();
        return;
    }
    if (!protectKeyWithPassword) {
        auto ctx = QGpgME::Job::context(keyGenJob);
        ctx->setPassphraseProvider(&emptyPassphraseProvider);
        ctx->setPinentryMode(Context::PinentryLoopback);
    }

    auto settings = KleopatraApplication::instance()->distributionSettings();
    if (settings) {
        keyParameters.setComment(settings->value(QStringLiteral("uidcomment"), {}).toString());
    }

    if (auto settings = Settings{}; !settings.designatedRevoker().isEmpty()) {
        keyParameters.addDesignatedRevoker(settings.designatedRevoker());
    }

    connect(keyGenJob, &QGpgME::KeyGenerationJob::result, q, [this](const KeyGenerationResult &result) {
        handleKeyGenerationResult(result);
    });

    if (const Error err = keyGenJob->start(keyParameters.toString())) {
        error(i18n("Could not start key pair creation: %1", Formatting::errorAsString(err)));
        finished();
        return;
    } else {
        job = keyGenJob;
    }
    progressDialog = new QProgressDialog;
    progressDialog->setAttribute(Qt::WA_DeleteOnClose);
    applyWindowID(progressDialog);
    progressDialog->setModal(true);
    progressDialog->setWindowTitle(i18nc("@title", "Creating Key Pair..."));
    progressDialog->setLabelText(i18n("The process of creating a key requires large amounts of random numbers. This may require several minutes..."));
    progressDialog->setRange(0, 0);
    connect(progressDialog, &QProgressDialog::canceled, job, &QGpgME::Job::slotCancel);
    connect(job, &QGpgME::Job::done, q, [this]() {
        if (progressDialog) {
            progressDialog->accept();
        }
    });
    progressDialog->show();
}

void NewOpenPGPCertificateCommand::Private::showResult(const KeyGenerationResult &result, const Key &key, const GpgME::Error &adskError)
{
    if (!key.isNull()) {
        if (adskError.code() && !adskError.isCanceled()) {
            success(
                xi18n("<para>A new OpenPGP certificate was created successfully, but adding an ADSK failed: %1</para>"
                      "<para>Fingerprint of the new certificate: %2</para>",
                      Formatting::errorAsString(adskError),
                      Formatting::prettyID(key.primaryFingerprint())));
        } else {
            success(
                xi18n("<para>A new OpenPGP certificate was created successfully.</para>"
                      "<para>Fingerprint of the new certificate: %1</para>",
                      Formatting::prettyID(key.primaryFingerprint())));
        }
        finished();
    } else {
        showErrorDialog(result);
    }
}

void NewOpenPGPCertificateCommand::Private::showErrorDialog(const KeyGenerationResult &result)
{
    QString text;
    if (result.error() || !result.fingerprint()) {
        text = xi18n(
            "<para>The creation of a new OpenPGP certificate failed.</para>"
            "<para>Error: <message>%1</message></para>",
            Formatting::errorAsString(result.error()));
    } else {
        // no error and we have a fingerprint, but there was no corresponding key in the key ring
        text = xi18n(
            "<para>A new OpenPGP certificate was created successfully, but it has not been found in the key ring.</para>"
            "<para>Fingerprint of the new certificate:<nl/>%1</para>",
            Formatting::prettyID(result.fingerprint()));
    }

    auto dialog = new QDialog;
    applyWindowID(dialog);
    dialog->setWindowTitle(i18nc("@title:window", "Error"));
    auto buttonBox = new QDialogButtonBox{QDialogButtonBox::Retry | QDialogButtonBox::Ok, dialog};
    const auto buttonCode = KMessageBox::createKMessageBox(dialog, buttonBox, QMessageBox::Critical, text, {}, {}, nullptr, {});
    if (buttonCode == QDialogButtonBox::Retry) {
        QMetaObject::invokeMethod(
            q,
            [this]() {
                getCertificateDetails();
            },
            Qt::QueuedConnection);
    } else {
        finished();
    }
}

NewOpenPGPCertificateCommand::NewOpenPGPCertificateCommand()
    : NewOpenPGPCertificateCommand(nullptr, nullptr)
{
}

NewOpenPGPCertificateCommand::NewOpenPGPCertificateCommand(QAbstractItemView *v, KeyListController *c)
    : Command(v, new Private(this, c))
{
}

NewOpenPGPCertificateCommand::~NewOpenPGPCertificateCommand() = default;

void NewOpenPGPCertificateCommand::doStart()
{
    d->getCertificateDetails();
}

void NewOpenPGPCertificateCommand::doCancel()
{
    if (d->detailsDialog) {
        d->detailsDialog->close();
    }
    if (d->job) {
        d->job->slotCancel();
    }
}

void NewOpenPGPCertificateCommand::Private::addAdsk(const Key &key, const QString &fingerprint, const KeyGenerationResult &result)
{
    const auto backend = QGpgME::openpgp();
    if (!backend) {
        return;
    }

    const auto job = backend->quickJob();
    if (!job) {
        return;
    }

    job->startAddAdsk(key, fingerprint.toLatin1().data());
    connect(job, &QGpgME::QuickJob::result, q, [key, this, result](const auto &error) {
        QMetaObject::invokeMethod(
            q,
            [key, error, this, result] {
                showResult(result, key, error);
            },
            Qt::QueuedConnection);
    });
}

void NewOpenPGPCertificateCommand::Private::handleKeyGenerationResult(const KeyGenerationResult &result)
{
    if (result.error().isCanceled()) {
        finished();
        return;
    }

    // Ensure that we have the key in the cache
    Key key;
    if (!result.error().code() && result.fingerprint()) {
        std::unique_ptr<Context> ctx{Context::createForProtocol(OpenPGP)};
        if (ctx) {
            Error err;
            key = ctx->key(result.fingerprint(), err, /*secret=*/true);
            if (!key.isNull()) {
                KeyCache::mutableInstance()->insert(key);
            }
        }
    }

    adskfpr = Settings{}.mandatoryADSK();
    if (!adskfpr.isEmpty() && !result.error().code() && result.fingerprint() && !key.isNull()) {
        QMetaObject::invokeMethod(
            q,
            [this, result, key] {
                addAdsk(key, adskfpr, result);
            },
            Qt::QueuedConnection);
        return;
    }
    QMetaObject::invokeMethod(
        q,
        [this, result, key] {
            showResult(result, key, GpgME::Error());
        },
        Qt::QueuedConnection);
}

#undef d
#undef q

#include "moc_newopenpgpcertificatecommand.cpp"
