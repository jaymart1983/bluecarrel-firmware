#pragma once

// Device-level power actions that live in main.cpp, declared here so an
// activity can reach them the way SilentRestart.h exposes the restart paths.

// Deep sleep. fromTimeout distinguishes the inactivity timer from a deliberate
// request, which the sleep-screen mode reads.
void enterDeepSleep(bool fromTimeout = false);

// Ask for deep sleep at the top of the next loop() instead of right now.
// enterDeepSleep() replaces the whole activity stack, so an activity must never
// call it from inside its own loop() or event handler -- it would be destroyed
// with its handler still on the stack. Used by the control centre's Sleep tile.
void requestDeviceSleep();

// Hand the SD card to the host as a USB mass-storage volume, at the top of the
// next loop(). Requested by the control centre's USB Drive tile.
//
// Deferred for the same reason as sleep, and for one more: entering drive mode
// paints a full-screen notice and then DETACHES the filesystem the fonts are
// read from. Doing that from inside an activity's event handler would tear the
// card out from under the screen that is still drawing itself.
void requestUsbDriveMode();

// True when a cable is present and the card could be handed over. The control
// centre shows its USB Drive tile only then -- an always-present tile that
// usually cannot do anything is worse than no tile.
bool usbDriveAvailable();
