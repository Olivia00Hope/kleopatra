/*  smartcard/card.h

    This file is part of Kleopatra, the KDE keymanager
    SPDX-FileCopyrightText: 2017 Bundesamt für Sicherheit in der Informationstechnik
    SPDX-FileContributor: Intevation GmbH

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "card.h"

#include "readerstatus.h"

#include "kleopatra_debug.h"

using namespace Kleo;
using namespace Kleo::SmartCard;

namespace {
static QString formatVersion(int value)
{
    if (value < 0) {
        return QString();
    }

    const unsigned int a = ((value >> 24) & 0xff);
    const unsigned int b = ((value >> 16) & 0xff);
    const unsigned int c = ((value >>  8) & 0xff);
    const unsigned int d = ((value      ) & 0xff);
    if (a) {
        return QStringLiteral("%1.%2.%3.%4").arg(QString::number(a), QString::number(b), QString::number(c), QString::number(d));
    } else if (b) {
        return QStringLiteral("%1.%2.%3").arg(QString::number(b), QString::number(c), QString::number(d));
    } else if (c) {
        return QStringLiteral("%1.%2").arg(QString::number(c), QString::number(d));
    }
    return QString::number(d);
}
}

Card::Card()
{
}

Card::~Card()
{
}

void Card::setStatus(Status s)
{
    mStatus = s;
}

Card::Status Card::status() const
{
    return mStatus;
}

void Card::setSerialNumber(const std::string &sn)
{
    mSerialNumber = sn;
}

std::string Card::serialNumber() const
{
    return mSerialNumber;
}

QString Card::displaySerialNumber() const
{
    return mDisplaySerialNumber;
}

void Card::setDisplaySerialNumber(const QString &serialNumber)
{
    mDisplaySerialNumber = serialNumber;
}

std::string Card::appName() const
{
    return mAppName;
}

void Card::setAppName(const std::string &name)
{
    mAppName = name;
}

void Card::setAppVersion(int version)
{
    mAppVersion = version;
}

int Card::appVersion() const
{
    return mAppVersion;
}

QString Card::displayAppVersion() const
{
    return formatVersion(mAppVersion);
}

std::string Card::cardType() const
{
    return mCardType;
}

int Card::cardVersion() const
{
    return mCardVersion;
}

QString Card::displayCardVersion() const
{
    return formatVersion(mCardVersion);
}

QString Card::cardHolder() const
{
    return mCardHolder;
}

void Card::setSigningKeyRef(const std::string &keyRef)
{
    mSigningKeyRef = keyRef;
}

std::string Card::signingKeyRef() const
{
    return mSigningKeyRef;
}

bool Card::hasSigningKey() const
{
    return !keyInfo(mSigningKeyRef).grip.empty();
}

void Card::setEncryptionKeyRef(const std::string &keyRef)
{
    mEncryptionKeyRef = keyRef;
}

std::string Card::encryptionKeyRef() const
{
    return mEncryptionKeyRef;
}

bool Card::hasEncryptionKey() const
{
    return !keyInfo(mEncryptionKeyRef).grip.empty();
}

std::vector<Card::PinState> Card::pinStates() const
{
    return mPinStates;
}

void Card::setPinStates(const std::vector<PinState> &pinStates)
{
    mPinStates = pinStates;
}

bool Card::hasNullPin() const
{
    return mHasNullPin;
}

void Card::setHasNullPin(bool value)
{
    mHasNullPin = value;
}

bool Card::canLearnKeys() const
{
    return mCanLearn;
}

void Card::setCanLearnKeys(bool value)
{
    mCanLearn = value;
}

bool Card::operator == (const Card &other) const
{
    return mStatus == other.status()
        && mSerialNumber == other.serialNumber()
        && mAppName == other.appName()
        && mAppVersion == other.appVersion()
        && mPinStates == other.pinStates()
        && mCanLearn == other.canLearnKeys()
        && mHasNullPin == other.hasNullPin()
        && mCardInfo == other.mCardInfo;
}

bool Card::operator != (const Card &other) const
{
    return !operator==(other);
}

void Card::setErrorMsg(const QString &msg)
{
    mErrMsg = msg;
}

QString Card::errorMsg() const
{
    return mErrMsg;
}

void Card::setInitialKeyInfos(const std::vector<KeyPairInfo> &infos)
{
    mKeyInfos = infos;
}

const std::vector<KeyPairInfo> & Card::keyInfos() const
{
    return mKeyInfos;
}

const KeyPairInfo & Card::keyInfo(const std::string &keyRef) const
{
    static const KeyPairInfo nullKey;
    for (const KeyPairInfo &k : mKeyInfos) {
        if (k.keyRef == keyRef) {
            return k;
        }
    }
    return nullKey;
}

void Card::setCardInfo(const std::vector<std::pair<std::string, std::string>> &infos)
{
    qCDebug(KLEOPATRA_LOG) << "Card" << serialNumber().c_str() << "info:";
    for (const auto &pair: infos) {
        qCDebug(KLEOPATRA_LOG) << pair.first.c_str() << ":" << pair.second.c_str();
        parseCardInfo(pair.first, pair.second);
    }
    processCardInfo();
}

namespace {
static int parseHexEncodedVersionTuple(const std::string &s) {
    // s is a hex-encoded, unsigned int-packed version tuple,
    // i.e. each byte represents one part of the version tuple
    bool ok;
    const auto version = QByteArray::fromStdString(s).toUInt(&ok, 16);
    return ok ? version : -1;
}
}

void Card::parseCardInfo(const std::string &name, const std::string &value)
{
    if (name == "APPVERSION") {
        mAppVersion = parseHexEncodedVersionTuple(value);
    } else if (name == "CARDTYPE") {
        mCardType = value;
    } else if (name == "CARDVERSION") {
        mCardVersion = parseHexEncodedVersionTuple(value);
    } else if (name == "DISP-NAME") {
        auto list = QString::fromUtf8(QByteArray::fromStdString(value)).
                    split(QStringLiteral("<<"), Qt::SkipEmptyParts);
        std::reverse(list.begin(), list.end());
        mCardHolder = list.join(QLatin1Char(' '));
    } else if (name == "KEYPAIRINFO") {
        const KeyPairInfo info = KeyPairInfo::fromStatusLine(value);
        if (info.grip.empty()) {
            qCWarning(KLEOPATRA_LOG) << "Invalid KEYPAIRINFO status line" << QString::fromStdString(value);
            setStatus(Card::CardError);
        } else {
            updateKeyInfo(info);
        }
    } else if (name == "KEY-FPR") {
        // handle OpenPGP key fingerprints
        const auto values = QString::fromStdString(value).split(QLatin1Char(' '));
        if (values.size() < 2) {
            qCWarning(KLEOPATRA_LOG) << "Invalid KEY-FPR status line" << QString::fromStdString(value);
            setStatus(Card::CardError);
        }
        const auto keyNumber = values[0];
        const std::string keyRef = "OPENPGP." + keyNumber.toStdString();
        const auto fpr = values[1].toStdString();
        if (keyNumber == QLatin1Char('1') || keyNumber == QLatin1Char('2') || keyNumber == QLatin1Char('3')) {
            addCardInfo("KLEO-FPR-" + keyRef, fpr);
        } else {
            // Maybe more keyslots in the future?
            qCDebug(KLEOPATRA_LOG) << "Unhandled keyslot" << keyNumber;
        }
    } else {
        mCardInfo.insert({name, value});
    }
}

void Card::processCardInfo()
{
}

void Card::addCardInfo(const std::string &name, const std::string &value)
{
    mCardInfo.insert({name, value});
}

std::string Card::cardInfo(const std::string &name) const
{
    const auto range = mCardInfo.equal_range(name);
    return range.first != range.second ? range.first->second : std::string();
}

void Card::updateKeyInfo(const KeyPairInfo& keyPairInfo)
{
    for (KeyPairInfo &k : mKeyInfos) {
        if (k.keyRef == keyPairInfo.keyRef) {
            k.update(keyPairInfo);
            return;
        }
    }
    mKeyInfos.push_back(keyPairInfo);
}
