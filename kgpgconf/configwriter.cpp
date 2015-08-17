/* -*- mode: c++; c-basic-offset:4 -*-
    configwriter.cpp

    This file is part of Kleopatra, the KDE keymanager
    Copyright (c) 2007 Klarälvdalens Datakonsult AB

    Kleopatra is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kleopatra is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

    In addition, as a special exception, the copyright holders give
    permission to link the code of this program with any edition of
    the Qt library by Trolltech AS, Norway (or with modified versions
    of Qt that use the same license as Qt), and distribute linked
    combinations including the two.  You must obey the GNU General
    Public License in all respects for all of the code used other than
    Qt.  If you modify this file, you may extend this exception to
    your version of the file, but you are not obligated to do so.  If
    you do not wish to do so, delete this exception statement from
    your version.
*/

#include "configwriter.h"

#include "configuration.h"

#include <QDateTime>
#include <QIODevice>
#include <QString>
#include <QTextStream>
#include <QVector>

#include <cassert>

namespace
{
struct GpgConfConfEntry {
    GpgConfConfEntry() : changeFlag(ConfigEntry::UnspecifiedMutability), useDefault(false) {}

    QString component;
    QString option;
    ConfigEntry::Mutability changeFlag;
    bool useDefault;
    QString value;
    QString toString() const
    {
        QString changeStr;
        if (changeFlag == ConfigEntry::Change) {
            changeStr = QStringLiteral("[change]");
        } else if (changeFlag == ConfigEntry::NoChange) {
            changeStr = QStringLiteral("[no-change]");
        }

        QString str;
        if (useDefault) {
            str += QStringLiteral("   %1 %2 [default]\n").arg(component, option);
        }
        if (!(useDefault && changeStr.isEmpty())) {
            str += QStringLiteral("   %1 %2 %3 %4\n").arg(component, option, changeStr, value);
        }
        return str;
    }
};
}

class ConfigWriter::Private
{
public:
    QIODevice *device;
    bool write(const QString &line);

};

ConfigWriter::ConfigWriter(QIODevice *device) : d(new Private)
{
    assert(device);
    d->device = device;
}

ConfigWriter::~ConfigWriter()
{
    delete d;
}

bool ConfigWriter::writeConfig(Config *config) const
{
    assert(d->device->isOpen());
    assert(d->device->isWritable());

    QVector<GpgConfConfEntry> lines;

    Q_FOREACH (const QString &i, config->componentList()) {
        ConfigComponent *const component = config->component(i);
        assert(component);
        Q_FOREACH (const QString &j, component->groupList()) {
            ConfigGroup *const group = component->group(j);
            assert(group);
            Q_FOREACH (const QString &k, group->entryList()) {
                ConfigEntry *const entry = group->entry(k);
                assert(entry);
                if (entry->mutability() != ConfigEntry::UnspecifiedMutability || entry->useBuiltInDefault() || !entry->outputString().isEmpty()) {
                    GpgConfConfEntry cfgentry;
                    cfgentry.useDefault = entry->useBuiltInDefault();
                    cfgentry.value = entry->useBuiltInDefault() ? QString() : entry->outputString();
                    cfgentry.component = component->name();
                    cfgentry.option = entry->name();
                    cfgentry.changeFlag = entry->mutability();
                    lines.append(cfgentry);
                }
            }
        }
    }

    QTextStream out(d->device);
    out << QStringLiteral("# gpgconf configuration file generated by kgpgconf on %1\n\n").arg(QDateTime::currentDateTime().toString());

    out << "*";
    Q_FOREACH (const GpgConfConfEntry &i, lines) {
        out << i.toString();
    }
    return true;
}

