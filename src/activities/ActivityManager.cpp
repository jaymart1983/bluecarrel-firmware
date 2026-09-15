#include "ActivityManager.h"

#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/MoreMenuActivity.h"
#include "home/RecentBooksActivity.h"
#include "network/UsbDriveActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/SdFirmwareUpdateActivity.h"
#include "settings/SettingsActivity.h"
#if FREEINK_CAP_BLE_TRANSFER
#include "network/BleStoreActivity.h"
#include "settings/BlePairingActivity.h"
#endif
#if FREEINK_CAP_NETWORK
#include "OpdsServerStore.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "network/NetworkModeSelectionActivity.h"
#include "settings/OpdsServerListActivity.h"
#endif
#include "util/BmpViewerActivity.h"
#include "util/FrontlightPanelActivity.h"
#include "util/FullScreenMessageActivity.h"

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

void ActivityManager::begin() {
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          8192,               // Stack size
                          this,               // Parameters
                          1,                  // Priority
                          &renderTaskHandle,  // Task handle
                          renderTaskCore  // Keep long renders/cover decodes off CPU 0's idle watchdog when available
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    // The persistent on-screen Back chip is opt-in per frame: clear it here so
    // only an activity whose render actually paints it (via drawButtonHints)
    // exposes a Back hit rect to the loop task.
    BaseTheme::setTouchBackButtonVisible(false);
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      // Night mode is a global output polarity applied to every activity.
      // The sleep screen forces normal polarity itself (SleepActivity).
      display.setInverted(SETTINGS.screenInverted != 0);
      const unsigned long renderStartMs = millis();
      currentActivity->render(std::move(lock));
      renderMillis.fetch_add(static_cast<uint32_t>(millis() - renderStartMs), std::memory_order_relaxed);
      completedRenders.fetch_add(1, std::memory_order_relaxed);
    }
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::loop() {
  if (mappedInput.consumeSuppressedRelease()) return;

  if (currentActivity && currentActivity->requiresExclusiveStorageLoop()) {
    currentActivity->loop();
    // An exclusive-storage activity must restart rather than navigate away:
    // processing a pending action here could re-enable filesystem users while
    // the USB host still owns the raw SD card.
    if (requestedUpdate.exchange(false) && renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
    return;
  }

  if (currentActivity) {
    if (!currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {
      if (currentActivity->handleHomeGesture()) {
        return;
      }
      goHome();
      return;
    }

    // Control-center entry, button route: a POWER TAP, from any ordinary screen
    // including the reader page. This is the one path that does not need the
    // glass, so it is what makes turning the touchscreen off recoverable -- the
    // tap opens the panel, the side keys move the tile cursor and a Right-key
    // hold selects. A ~1 s power hold closes it again. Both gestures are
    // classified in main.cpp's decoder; here they only have to be routed.
    const bool panelUp = currentActivity->name == "FrontlightPanel";
    const bool panelRoutable =
        mappedInput.hasTouch() && currentActivity->name != "Boot" && currentActivity->name != "Sleep";
    if (panelRoutable && mappedInput.consumeControlCenterClose()) {
      // Hold closes and only closes: with nothing open it is deliberately inert,
      // so a hold can never be the thing that opens the menu it is meant to shut.
      if (panelUp) {
        popActivity();
        return;
      }
    }
    // A power tap toggles: it opens the panel, or closes the one that is up. The
    // hold that used to close it is Sleep now (main.cpp's decoder).
    if (panelRoutable && mappedInput.consumeControlCenterOpen()) {
      if (panelUp) {
        popActivity();
      } else {
        pushActivity(std::make_unique<FrontlightPanelActivity>(renderer, mappedInput));
      }
      return;
    }

    // Control-center entry: a tap on the TOP-CENTRE of the status-bar band of
    // the top-level screens opens it. Deliberately touch-only and centre-only:
    // the top-edge down-swipe that used to open it as well is gone (it fought
    // the etched glass, and a swipe that opens a menu is undiscoverable), and
    // the left/right thirds of the band stay free for a screen's own chrome.
    //
    // The reader is on this list now. It used to be excluded because it had no
    // status bar to tap -- its chrome was at the foot of the page. The band is
    // at the head of the page now (BaseTheme::drawReaderTopBar), so the gesture
    // has the same target here as everywhere else, and the one screen the user
    // spends all their time on was the one screen it did not work on.
    bool controlCenterTap = false;
    if (mappedInput.hasTouch() &&
        (currentActivity->name == "Home" || currentActivity->name == "FileBrowser" ||
         currentActivity->name == "Settings" || currentActivity->name == "BlePairing" ||
         currentActivity->name == "NetworkModeSelection" || currentActivity->name == "EpubReader" ||
         currentActivity->name == "FirmwareReady")) {
      int tx = 0;
      int ty = 0;
      const int width = renderer.getScreenWidth();
      // The band's own height rather than a hard-coded 44, so the target is
      // exactly the strip the user can see.
      controlCenterTap = mappedInput.wasScreenTapped(tx, ty) && ty < BaseTheme::readerTopBarHeight() &&
                         tx >= width / 3 && tx < (width * 2) / 3;
    }
    if (currentActivity->name != "FrontlightPanel" && controlCenterTap) {
      pushActivity(std::make_unique<FrontlightPanelActivity>(renderer, mappedInput));
      return;
    }

    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate.exchange(false)) {
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

#if FREEINK_CAP_NETWORK
// WiFi file transfer only. There is no File Transfer screen on a network-less
// board any more: USB Drive mounts itself when a cable arrives (see the plug-edge
// handler in main.cpp) and Bluetooth is up whenever the device is, so nothing
// was left for a menu to start.
void ActivityManager::goToFileTransfer() {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput));
}
#endif

void ActivityManager::goToUsbDrive(const bool automatic) {
#if FREEINK_CAP_USB_MSC
  auto activity = makeUniqueNoThrow<UsbDriveActivity>(renderer, mappedInput, automatic);
  if (!activity) {
    LOG_ERR("ACT", "OOM: USB Drive activity");
    return;
  }
  replaceActivity(std::move(activity));
#else
  (void)automatic;
  LOG_ERR("ACT", "USB Drive requested in a build without USB Drive capability");
#endif
}

#if FREEINK_CAP_BLE_TRANSFER
void ActivityManager::goToStore() {
  // -fno-exceptions: make_unique would abort on OOM, so match the no-throw
  // pattern the rest of this file moved to.
  auto activity = makeUniqueNoThrow<BleStoreActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: Store activity");
    return;
  }
  replaceActivity(std::move(activity));
}
#endif

void ActivityManager::goToFirmwareUpdate(std::string path, const bool stagedDrop, const bool autoConfirm,
                                         const bool sleepAfter) {
  auto activity = makeUniqueNoThrow<SdFirmwareUpdateActivity>(renderer, mappedInput, std::move(path), stagedDrop,
                                                              autoConfirm, sleepAfter);
  if (!activity) {
    LOG_ERR("ACT", "OOM: firmware update activity");
    return;
  }
  // replaceActivity, not push: an update reboots the device, so there is nothing
  // for a stack underneath it to come back to.
  replaceActivity(std::move(activity));
}

void ActivityManager::goToMoreMenu() { replaceActivity(std::make_unique<MoreMenuActivity>(renderer, mappedInput)); }

void ActivityManager::goToSettings() { replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInput)); }

#if FREEINK_CAP_BLE_TRANSFER
void ActivityManager::goToBlePairing() {
  replaceActivity(std::make_unique<BlePairingActivity>(renderer, mappedInput));
}
#else
void ActivityManager::goToBlePairing() { goToSettings(); }
#endif

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToRecentBooks() {
  replaceActivity(std::make_unique<RecentBooksActivity>(renderer, mappedInput));
}

#if FREEINK_CAP_NETWORK
void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}
#endif

void ActivityManager::goToReader(std::string path) {
  if (path.empty()) {
    goToFileBrowser("/");
    return;
  }

  if (FsHelpers::hasBmpExtension(path) || FsHelpers::hasPngExtension(path)) {
    auto activity = makeUniqueNoThrow<BmpViewerActivity>(renderer, mappedInput, std::move(path));
    if (!activity) {
      LOG_ERR("ACT", "OOM: bitmap viewer activity");
      return;
    }
    replaceActivity(std::move(activity));
    return;
  }

  auto activity = ReaderActivity::create(renderer, mappedInput, std::move(path));
  if (activity) {
    replaceActivity(std::move(activity));
  }
}

void ActivityManager::goToSleep(bool fromTimeout) {
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput, fromTimeout));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem, bool cleanInitialRefresh) {
  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    // Back goes back ONE level, not to the top. Settings is opened from More, so
    // leaving it belongs on More -- landing on Home with the selector parked on
    // the More row looks like Back skipped a screen, because it did.
    //
    // Activities are replaced rather than stacked here (only one is resident at
    // a time), so "the previous screen" cannot be popped and has to be named.
    if (activityName == "Settings") {
      goToMoreMenu();
      return;
    }
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "RecentBooks") {
      initialMenuItem = HomeMenuItem::RECENTS;
#if FREEINK_CAP_NETWORK
    } else if (activityName == "OpdsBookBrowser") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
#endif
    } else if (activityName == "Store") {
      initialMenuItem = HomeMenuItem::STORE;
    } else if (activityName == "MoreMenu") {
      initialMenuItem = HomeMenuItem::MORE;
    } else if (activityName == "Settings") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem, cleanInitialRefresh));
}
void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::requiresExclusiveStorageLoop() const {
  return currentActivity && currentActivity->requiresExclusiveStorageLoop();
}

bool ActivityManager::isReaderActivity() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); }) ||
         (currentActivity && currentActivity->isReaderActivity());
}

bool ActivityManager::isReaderPageActive() const { return currentActivity && currentActivity->isReaderActivity(); }

bool ActivityManager::handleForcedRefresh() { return currentActivity && currentActivity->handleForcedRefresh(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
void ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityManagerSpinlock);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  xTaskNotify(renderTaskHandle, 1, eIncrement);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// RenderLock

RenderLock::RenderLock() {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
