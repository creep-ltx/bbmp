// SPDX-License-Identifier: GPL-2.0-only
// bbmp-tiatracker: stands in for TIATracker's mainwindow.h. The song code needs the
// song file version it knows (1, as TIATracker 1.3) and a place for its messages.
#ifndef MAINWINDOW_H
#define MAINWINDOW_H
#include "qtshim.h"
#include "emulation/player.h"

class MainWindow {
public:
    static const int version = 1;
    // The song code's last complaint, which the helper reports as the error
    static QString lastMessage;
    static void displayMessage(const QString &message) { lastMessage = message; }
};
#endif
