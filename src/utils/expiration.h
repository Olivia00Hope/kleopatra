/* -*- mode: c++; c-basic-offset:4 -*-
    utils/expiration.h

    This file is part of Kleopatra, the KDE keymanager
    SPDX-FileCopyrightText: 2023 g10 Code GmbH
    SPDX-FileContributor: Ingo Klöcker <dev@ingo-kloecker.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QDate>

class KDateComboBox;

namespace Kleo
{
    struct DateRange {
        QDate minimum;
        QDate maximum;
    };

    /**
     * Returns a date a bit before the technically possible latest expiration
     * date (~2106-02-07) that is safe to use as latest expiration date.
     */
    QDate maximumAllowedDate();

    /**
     * Returns the earliest allowed expiration date.
     *
     * This is either tomorrow or the configured number of days after today
     * (whichever is later).
     *
     * \sa Settings::validityPeriodInDaysMin
     */
    QDate minimumExpirationDate();

    /**
     * Returns the latest allowed expiration date.
     *
     * If unlimited validity is allowed, then an invalid date is returned.
     * Otherwise, either the configured number of days after today or
     * the maximum allowed date, whichever is earlier, is returned.
     * Additionally, the returned date is never earlier than the minimum
     * expiration date.
     *
     * \sa Settings::validityPeriodInDaysMax
     */
    QDate maximumExpirationDate();

    /**
     * Returns the allowed range for the expiration date.
     *
     * \sa minimumExpirationDate, maximumExpirationDate
     */
    DateRange expirationDateRange();

    enum class ExpirationOnUnlimitedValidity {
        NoExpiration,
        InternalDefaultExpiration,
    };

    /**
     * Returns a useful value for the default expiration date based on the current
     * date and the configured default validity. If the configured validity is
     * unlimited, then the return value depends on \p onUnlimitedValidity.
     *
     * The returned value is always in the allowed range for the expiration date.
     *
     * \sa expirationDateRange
     */
    QDate defaultExpirationDate(ExpirationOnUnlimitedValidity onUnlimitedValidity);

    /**
     * Configures the date combo box \p dateCB for choosing an expiration date.
     *
     * Sets the allowed date range and a tooltip. And disables the combo box
     * if a fixed validity period is configured.
     */
    void setUpExpirationDateComboBox(KDateComboBox *dateCB);
}