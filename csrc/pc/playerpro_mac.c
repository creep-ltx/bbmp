// SPDX-License-Identifier: GPL-3.0-or-later
// On macOS PlayerPRO's driver asks for its CoreAudio output and a Finder file type, from
// files the player doesn't build: it only renders into memory, so they are never used.
#include "MADPrivate.h"

MADErr initCoreAudio(MADDriverRec* inMADDriver) { (void)inMADDriver; return MADParametersErr; }
MADErr closeCoreAudio(MADDriverRec* inMADDriver) { (void)inMADDriver; return MADNoErr; }
void SetOSType(CFURLRef url, OSType type) { (void)url; (void)type; }
