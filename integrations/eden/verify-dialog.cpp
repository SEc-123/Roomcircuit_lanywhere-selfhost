// SPDX-FileCopyrightText: Copyright 2026 RoomCircuit contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <functional>
#include <QApplication>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include "roomcircuit_dialog.h"

// Run only against a fresh, disposable RoomCircuit relay. Creates an appointment.
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    if (argc != 4) {
        qCritical("usage: verify-dialog SERVICE-ORIGIN EXPECTED-EDEN-HOST EXPECTED-EDEN-PORT");
        return 2;
    }
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Eden"),
                       QStringLiteral("RoomCircuit"));
    settings.setValue(QStringLiteral("origin"), QString::fromLocal8Bit(argv[1]));
    settings.sync();
    int step = 0;
    bool completed = false;
    auto* deadline = new QTimer(&application);
    std::function<void()> drive;
    drive = [&] {
        QTimer::singleShot(60, &application, drive);
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        auto click = [&](const QString& text) {
            for (auto* button : dialog->findChildren<QPushButton*>()) {
                if (button->text() == text && button->isEnabled() && button->isVisible()) {
                    ++step;
                    button->click();
                    break;
                }
            }
        };
        switch (step) {
        case 0:
            click(QStringLiteral("New appointment"));
            break;
        case 1:
            if (auto* time = dialog->findChild<QDateTimeEdit*>()) {
                auto* actions = dialog->findChild<QDialogButtonBox*>();
                time->setDateTime(QDateTime::currentDateTime().addSecs(600));
                ++step;
                actions->button(QDialogButtonBox::Ok)->click();
            }
            break;
        case 2:
            click(QStringLiteral("Follow"));
            break;
        case 3:
            for (auto* button : dialog->findChildren<QPushButton*>()) {
                if (button->text() == QStringLiteral("Stop following")) {
                    click(QStringLiteral("Mark ready"));
                    break;
                }
            }
            break;
        case 4:
            for (auto* button : dialog->findChildren<QPushButton*>()) {
                if (button->text() == QStringLiteral("Cancel ready")) {
                    click(QStringLiteral("Find rooms"));
                    break;
                }
            }
            break;
        case 5:
            for (auto* label : dialog->findChildren<QLabel*>()) {
                if (label->text() == QStringLiteral("No compatible open rooms for this appointment.")) {
                    click(QStringLiteral("Cancel ready"));
                    break;
                }
            }
            break;
        case 6:
            for (auto* button : dialog->findChildren<QPushButton*>()) {
                if (button->text() == QStringLiteral("Mark ready")) {
                    click(QStringLiteral("Direct connect…"));
                    break;
                }
            }
            break;
        }
    };
    deadline->setSingleShot(true);
    QObject::connect(deadline, &QTimer::timeout, &application, [&] {
        qCritical("RoomCircuit dialog timed out at step %d", step);
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            dialog->reject();
        }
        application.exit(1);
    });
    deadline->start(20000);
    QTimer::singleShot(0, &application, drive);
    ShowRoomCircuit(nullptr, [&](const QString& host, int port) {
        completed = step == 7 && host == QString::fromLocal8Bit(argv[2]) &&
                    port == QString::fromLocal8Bit(argv[3]).toInt();
    });
    if (!completed) {
        qCritical("Native appointment flow failed at step %d", step);
        return 1;
    }
    qInfo("Desktop native flow verified: load, create, follow, ready, find rooms, cancel ready, direct-connect endpoint.");
    return 0;
}
