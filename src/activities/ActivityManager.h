#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "util/ScreenshotInfo.h"

class Activity;    // forward declaration
class RenderLock;  // forward declaration

// OPDS_BROWSER is kept unconditionally even on FREEINK_CAP_NETWORK=0 builds:
// it is a transient "which row was I on" hint, never persisted, and HomeActivity
// simply never produces it when the OPDS row is not built.
//
// STORE and MORE were appended rather than inserted for the same reason: the
// values are ordinary enumerators that other code switches on, and appending
// keeps every existing case label meaning what it did.
enum class HomeMenuItem { NONE, FILE_BROWSER, RECENTS, OPDS_BROWSER, FILE_TRANSFER, SETTINGS_MENU, STORE, MORE };

/**
 * ActivityManager
 *
 * This mirrors the same concept of Activity in Android, where an activity represents a single screen of the UI. The
 * manager is responsible for launching activities, and ensuring that only one activity is active at a time.
 *
 * It also provides a stack mechanism to allow activities to launch sub-activities and get back the results when the
 * sub-activity is done. For example, the WebServer activity can launch a WifiSelect activity to let the user choose a
 * wifi network, and get back the selected network when the user is done.
 *
 * Main differences from Android's ActivityManager:
 * - No onPause/onResume, since we don't have a concept of background activities
 * - onActivityResult is implemented via a callback instead of a separate method, for simplicity
 */
class ActivityManager {
  friend class RenderLock;

 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  std::unique_ptr<Activity> currentActivity;

  void exitActivity(const RenderLock& lock);

  // Pending activity to be launched on next loop iteration
  std::unique_ptr<Activity> pendingActivity;
  enum class PendingAction { None, Push, Pop, Replace };
  PendingAction pendingAction = PendingAction::None;

  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

  // Set by requestUpdateAndWait(); read and cleared by the render task after render completes.
  // Note: only one waiting task is supported at a time
  TaskHandle_t waitingTaskHandle = nullptr;

  // Mutex to protect rendering operations from race conditions
  // Must only be used via RenderLock
  SemaphoreHandle_t renderingMutex = nullptr;

  // Whether to trigger a render after the current loop()
  // This variable must only be set by the main loop, to avoid race conditions
  std::atomic<bool> requestedUpdate{false};

 public:
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
    assert(renderingMutex != nullptr && "Failed to create rendering mutex");
    stackActivities.reserve(10);
  }
  ~ActivityManager() { assert(false); /* should never be called */ };

  void begin();
  void loop();

  // Will replace currentActivity and drop all activities on stack
  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  // goTo... functions are convenient wrapper for replaceActivity()
#if FREEINK_CAP_NETWORK
  // WiFi file transfer. Network-less boards have no such screen: USB Drive is
  // automatic and Bluetooth is always on.
  void goToFileTransfer();
#endif
  // USB mass storage. Entered automatically when a cable is plugged into an
  // awake device (see main.cpp), and from the WiFi transfer menu where there
  // still is one.
  void goToUsbDrive(bool automatic = false);
#if FREEINK_CAP_BLE_TRANSFER
  // The Calibre store, browsed over the always-on BLE link.
  void goToStore();
#endif
  // Validate, confirm and flash `path`. `stagedDrop` says the image came out of
  // the watched /firmware folder, which is the only case that clears that folder
  // afterwards.
  // autoConfirm: the user already said yes (Update now, or an install at sleep).
  // sleepAfter: reboot straight back into sleep once installed.
  void goToFirmwareUpdate(std::string path, bool stagedDrop, bool autoConfirm = false, bool sleepAfter = false);
  // Browse Files / Recent Books / File Transfer / Settings, demoted off the
  // home screen when home became a shelf.
  void goToMoreMenu();
  void goToSettings();
  // The device's only remaining configuration screen. Every other setting is the
  // app's now, read and written over BLE, so the Action Centre goes straight
  // here rather than through a settings tree that duplicates it.
  void goToBlePairing();
  void goToFileBrowser(std::string path = {});
  void goToRecentBooks();
#if FREEINK_CAP_NETWORK
  void goToBrowser();
#endif
  void goToReader(std::string path);
  void goToSleep(bool fromTimeout = false);
  void goToBoot();
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void goToCrashReport();
  void goHome(HomeMenuItem initialMenuItem = HomeMenuItem::NONE, bool cleanInitialRefresh = false);

  // This will move current activity to stack instead of deleting it
  void pushActivity(std::unique_ptr<Activity>&& activity);

  // Remove the currentActivity, returning the last one on stack
  // Note: if popActivity() on last activity on the stack, we will goHome()
  void popActivity();

  bool preventAutoSleep() const;
  bool requiresExclusiveStorageLoop() const;
  bool isReaderActivity() const;
  // True only when the reader PAGE itself has focus — a reader menu, popup or
  // the control centre pushed on top makes this false while isReaderActivity()
  // stays true. This is the "inside a book" test the input scheme keys off (see
  // MappedInputManager::setInBookContext): those pushed screens are exactly the
  // ones that need Back and Select on the side keys.
  bool isReaderPageActive() const;
  bool handleForcedRefresh();
  bool skipLoopDelay() const;
  ScreenshotInfo getScreenshotInfo() const;

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  void requestUpdate(bool immediate = false);

  // Trigger a render and block until it completes.
  // Must NOT be called from the render task or while holding a RenderLock.
  void requestUpdateAndWait();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
