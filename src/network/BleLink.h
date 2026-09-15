#pragma once

#include <BoardConfig.h>

#if FREEINK_CAP_BLE_TRANSFER

#include <HalStorage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "BleTrustedHostStore.h"
#include "util/BookProgressSync.h"

class BleStoreController;
struct BleLinkRuntime;

// The reader's Bluetooth link. One NimBLE peripheral, one hello/HMAC gate, one
// framed upload path with credit flow control and SHA-256, one download path.
//
// WHY THIS IS NOT AN ACTIVITY. The radio is a property of the device being awake,
// not of any one screen: the phone app must be able to reach a reader that is
// showing a book, and the Store must be able to ask the phone for a catalogue
// page whatever else is up. So the link lives here, is started once at boot
// (main.cpp) and is stopped on the way into deep sleep. Wake from deep sleep is a
// chip reset, so start-on-boot is also start-on-wake.
//
// WHAT A SCREEN DOES INSTEAD. A screen that wants to show link state registers
// as an Observer and is told when something changed; it repaints itself. The
// Store additionally attaches its controller so the request channel has
// somewhere to publish to. Neither owns the radio and neither can take it down.
//
// SECURITY (docs/security-v2.md). The link is LE Secure Connections with
// bonding and passkey MITM protection; nothing is read, written or notified
// until the one connection is encrypted, authenticated and bonded. A new bond
// is accepted only while BlePairingActivity holds the pairing window open. On
// top of the bond, the app proves the shared secret with `hello` (or stores it
// with `pair`) and the reader proves it back.
class BleLink {
 public:
  enum class State {
    STARTING,
    ADVERTISING,
    CONNECTED,
    RECEIVING,
    VERIFYING,
    SAVED,
    PREPARING,
    SENDING,
    SENT,
    ERROR
  };
  enum class TransferKind {
    NONE,
    BOOK,
    BMP,
    FIRMWARE,
    PROGRESS,
    CRASH_REPORT,
    LIBRARY,
    PROGRESS_RESULT,
    CATALOG_PAGE,
    CATALOG_DETAIL,
    // Not SETTINGS: that name is a macro for the settings singleton
    // (CrossPointSettings.h), and an enumerator by that name expands inside the
    // enum and takes the whole class declaration with it.
    BOOK_META,        ///< app -> device: a book's cover and metadata, sent ahead of the book
    SETTINGS_INBOX,   ///< app -> device: a settings document to apply
    SETTINGS_SNAPSHOT, ///< device -> app: the current settings document
    ABOUT              ///< device -> app: build identity (firmware version, running slot)
  };

  // A screen that paints something about the link. There is at most one: only
  // the frontmost activity has anything to repaint, and an observer that is not
  // on screen would be asking the render task to draw over whoever is.
  class Observer {
   public:
    virtual ~Observer() = default;
    virtual void onBleLinkChanged() = 0;
  };

  static BleLink& getInstance();

  // --- radio lifecycle -------------------------------------------------------
  // Start the peripheral and begin advertising. Idempotent; safe to call when
  // already running.
  void begin();
  // Stop advertising and take the stack down. Idempotent. Called on the way into
  // deep sleep, where the modem power domain must not be held alive.
  void end();
  bool isRunning() const { return ble_ != nullptr; }
  // Pump the events the NimBLE host task queued. Called once per main loop,
  // whatever is on screen.
  void tick();
  // The main loop's idle sleep: up to `ms`, returning early when a control
  // write (get_ack, commit, start_get ...) is queued, so the answer to it is not
  // held back by a fixed delay.
  void waitForWork(unsigned long ms);

  // --- pairing ---------------------------------------------------------------
  // The pairing window: a new bond, and the `pair` op, are accepted only while
  // it is open. Opened and closed by BlePairingActivity. Main loop only.
  void openPairingWindow();
  void closePairingWindow();
  // Requested and not locked out. Safe from the NimBLE host task.
  bool pairingWindowOpen() const;
  // Seconds left of the lockout after too many failed attempts; 0 when none.
  uint32_t pairingLockSecondsLeft() const;
  // The passkey the phone must type for the pairing in progress. False when no
  // pairing is in progress.
  bool pairingPasskey(uint32_t& passkey) const;

  bool hasTrustedHost() const;
  // The saved host's display name, or an empty string when nobody is paired.
  std::string trustedHostLabel() const;
  // Removes the trusted host and every bond.
  bool forgetTrustedHost();
  // Why the last `hello` or `pair` was refused, if it was. Never an error state.
  const std::string& authError() const { return authErrorMessage_; }
  // Drops the last refusal, so Settings shows no stale error. Main loop only.
  void clearAuthError();
  bool isPeerConnected() const;

  // --- pairing consent -------------------------------------------------------
  // A valid `pair` that arrives while the window is closed is held here until the
  // person holding the reader answers BlePairPromptActivity. Main loop only.
  //
  // True once per held request: main.cpp pushes the prompt when it sees it.
  bool takePairPromptRequest();
  bool pairPromptPending() const { return pendingPairActive_; }
  // The sanitised host_name of the held request.
  const std::string& pairPromptHostName() const { return pendingPair_.name; }
  // Allow applies the held request as if the window had been open; deny refuses
  // it with auth_error "pairing denied". Nothing happens when none is held.
  void resolvePairPrompt(bool allow);

  // The gate is open: a phone is connected AND authenticated.
  bool isAuthenticated() const { return sessionAuthenticated(); }
  // Advertises SETTINGS' device name (the default when blank) as the GAP name and
  // in the scan response. Advertising restarts to carry it only when no phone is
  // connected; with one connected, the next advertising start carries it. Nothing
  // happens when the name is unchanged, or while the radio is off (begin() reads
  // the setting). Main loop only.
  void applyDeviceName();

  // --- store hosting ---------------------------------------------------------
  // The Store screen lends its controller to the link for as long as it is on
  // screen. The link owns no catalogue and no UI; the controller owns no radio.
  void attachStore(BleStoreController* store);
  void detachStore(const BleStoreController* store);
  void armStoreBookFetch(const std::string& filename) { storeExpectedBook_ = filename; }
  void publishStatusNow();

  // --- heartbeat ("ping") -------------------------------------------------
  //
  // The reader is a peripheral: it cannot call the phone, it can only notify a
  // phone that is already connected. These are the moments worth notifying.
  //
  // Position changed -- a page turn, or a saved position. Cheap: re-reads one
  // eleven-byte sidecar, no book is opened.
  void notePositionChanged();
  // Something wrote to the card, so the library fingerprint is stale. Does not
  // recompute here; the next heartbeat pays for the walk.
  void noteLibraryChanged();
  // Last word before deep sleep. Without it the app cannot tell "asleep" from
  // "out of range" or "crashed"; it just sees the link drop. Waking is a chip
  // reset, so the first status after boot already reports the reader awake.
  void notifySleeping();

  // --- observers -------------------------------------------------------------
  void setObserver(Observer* observer) { observer_ = observer; }
  void clearObserver(const Observer* observer) {
    if (observer_ == observer) observer_ = nullptr;
  }

  State state() const { return state_; }
  const std::string& errorMessage() const { return errorMessage_; }
  const std::string& fileName() const { return fileName_; }
  // A transfer is in flight. The inactivity timer honours this, so a book
  // arriving while the reader sits on the home screen is not cut off halfway by
  // auto-sleep.
  bool isBusy() const {
    return transferOpen_ || downloadOpen_ || pendingCommit_ || state_ == State::VERIFYING ||
           state_ == State::PREPARING;
  }

  // --- NimBLE host-task entry points ----------------------------------------
  static constexpr uint16_t NO_CONNECTION = 0xFFFF;  // BLE_HS_CONN_HANDLE_NONE

  // Binds the link to `connHandle`. False when another connection is bound.
  bool bindConnection(uint16_t connHandle);
  // Releases the binding. False when `connHandle` is not the bound connection.
  bool releaseConnection(uint16_t connHandle);
  uint16_t boundConnection() const { return connHandle_.load(); }
  // The bound connection passed the security checks (encrypted, authenticated,
  // bonded, and inside the pairing window if the bond is new).
  bool boundConnectionSecure(uint16_t connHandle) const {
    return linkSecure_.load() && connHandle != NO_CONNECTION && connHandle == connHandle_.load();
  }
  // A passkey is on screen for the bound connection.
  void notePairingStarted(uint32_t passkey);
  // Clears the in-progress flag and returns whether it was set.
  bool takePairingStarted() { return pairingInProgress_.exchange(false); }
  // One failed attempt inside the window; the third locks pairing.
  void notePairingFailed();
  void markConnectionSecure() { linkSecure_.store(true); }

  void enqueueBleConnected(uint16_t connHandle);
  void enqueueBleDisconnected(uint16_t connHandle);
  // `peerIdAddress` is the raw ble_addr_t of the peer when the bond is new.
  void enqueueSecurityResult(uint16_t connHandle, bool accepted, bool newBond, const std::string& peerIdAddress);
  // By value: the caller's temporary is moved into the queue, one copy per frame.
  void enqueueControlWrite(uint16_t connHandle, std::string value);
  void enqueueDataWrite(uint16_t connHandle, std::string value);
  // Called from the NimBLE host task on connect and on every MTU exchange. Zero
  // means "nothing negotiated". This is only a fallback: notifyCapBytes() asks
  // the live connection what the MTU actually is and uses this when there is no
  // connection to ask.
  void noteBleMtu(uint16_t mtu);
  // Link parameters in force, from the NimBLE host task: on connect (`connected`,
  // which also resets PHY and data length to their LL defaults) and on every
  // connection update, PHY update and data length change. Each is logged and
  // reported as `link` in `about`. Ignored for any handle but the bound one.
  void noteConnParams(uint16_t connHandle, uint16_t intervalUnits, uint16_t latency, uint16_t timeoutUnits,
                      bool connected);
  void notePhy(uint16_t connHandle, uint8_t txPhy, uint8_t rxPhy);
  void noteDataLength(uint16_t connHandle, uint16_t txOctets, uint16_t txTimeUs, uint16_t rxOctets,
                      uint16_t rxTimeUs);
  // The most a notification may carry right now: ATT_MTU-3, never more than
  // BLE_STATUS_NOTIFY_MAX_BYTES. Public because the runtime sizes frames by it.
  size_t notifyCapBytes() const;
  // The most a data-out frame may carry right now: ATT_MTU-3, without the status
  // cap. Download chunks are sized from it.
  size_t dataNotifyCapBytes() const;

  // READ is the authoritative document a GATT read returns -- everything the
  // session knows. NOTIFY is the doorbell: the same document with the fields a
  // client can re-read dropped, so it fits an ATT payload without truncation.
  enum class StatusScope { READ, NOTIFY };
  // `detail` narrows a NOTIFY document; it is ignored for READ. See
  // STATUS_DETAIL_MAX in the .cpp for what each level keeps.
  std::string buildStatusJson(StatusScope scope, unsigned detail) const;

 private:
  friend struct BleLinkRuntime;

  BleLink() = default;

  // PAIRING: the pairing state changed (passkey shown, attempt failed, lockout).
  enum class BleEventType { CONNECTED, DISCONNECTED, SECURITY, PAIRING, CONTROL, DATA };
  struct BleEvent {
    BleEventType type;
    std::string value;
    uint16_t connHandle = NO_CONNECTION;
    bool accepted = false;
    bool newBond = false;
    unsigned long atMs = 0;  ///< millis() when the host task queued it
  };

  static constexpr uint8_t MAX_FAILED_PAIRINGS = 3;
  static constexpr unsigned long PAIRING_LOCK_MS = 60UL * 1000UL;
  static constexpr uint8_t MAX_INVALID_HELLOS = 3;
  // From encryption to an accepted `hello` or `pair`.
  static constexpr unsigned long HELLO_TIMEOUT_MS = 20UL * 1000UL;
  // From connection to a secure link; long enough to type a passkey.
  static constexpr unsigned long SECURE_LINK_TIMEOUT_MS = 90UL * 1000UL;
  // A held `pair` is refused when nobody answers the prompt within this.
  static constexpr unsigned long PAIR_PROMPT_TIMEOUT_MS = 60UL * 1000UL;
  // At most one prompt starts per interval; `pair` in between is refused.
  static constexpr unsigned long PAIR_PROMPT_INTERVAL_MS = 30UL * 1000UL;

  // The `pair` awaiting consent (secret included). Wiped by clearPendingPair().
  BleTrustedHost pendingPair_;
  bool pendingPairActive_ = false;
  // The connection that sent it is still the one up. Cleared by any connect or
  // disconnect, so an answer never authenticates a different connection.
  bool pendingPairLinkAlive_ = false;
  bool pairPromptRequested_ = false;
  bool pairPromptStarted_ = false;
  unsigned long pendingPairAtMs_ = 0;
  unsigned long lastPairPromptAtMs_ = 0;
  void clearPendingPair();
  // Stores `host` as the trusted host and closes the window; with
  // `authenticateSession`, also marks this connection paired and authenticated.
  bool applyPair(const BleTrustedHost& host, bool authenticateSession);

  // Shared with the NimBLE host task.
  std::atomic<uint16_t> connHandle_{NO_CONNECTION};
  std::atomic<bool> linkSecure_{false};
  std::atomic<bool> pairingWindowRequested_{false};
  std::atomic<bool> pairingInProgress_{false};
  std::atomic<uint32_t> passkey_{0};
  std::atomic<uint8_t> failedPairings_{0};
  std::atomic<bool> pairingLocked_{false};
  std::atomic<unsigned long> pairingLockedAtMs_{0};

  // Main loop only; reset per connection.
  // The connection an accepted hello or pair belongs to.
  uint16_t authHandle_ = NO_CONNECTION;
  bool sessionAuthenticated() const { return helloAccepted_ && authHandle_ == connHandle_.load(); }
  uint8_t invalidHellos_ = 0;
  unsigned long connectedAtMs_ = 0;
  unsigned long securedAtMs_ = 0;
  // HMAC proof of the secret for the last accepted hello, published in status.
  std::string readerProof_;

  // Borrowed from the Store screen for as long as that screen is up; null the
  // rest of the time, which is most of the time.
  BleStoreController* store_ = nullptr;
  Observer* observer_ = nullptr;
  // The one filename the store has armed a `book` upload for. A book upload in
  // store mode that names anything else is refused: the device asked for a
  // specific book and must not accept a different one in its place.
  std::string storeExpectedBook_;

  State state_ = State::STARTING;
  std::unique_ptr<BleLinkRuntime> ble_;
  HalFile uploadFile_;
  HalFile downloadFile_;
  SemaphoreHandle_t eventMutex_ = nullptr;
  // Binary semaphore given by the host task when a control write is queued;
  // waitForWork() blocks on it.
  SemaphoreHandle_t eventSignal_ = nullptr;
  std::deque<BleEvent> bleEvents_;
  size_t queuedBleEventBytes_ = 0;
  bool bleEventOverflow_ = false;

  std::string fileName_;
  // Build stamp (yyyyMMdd.HHmm) and signature (hex DER) sent with a firmware image.
  std::string firmwareVersion_;
  std::string firmwareSignature_;
  std::string partPath_;
  std::string finalPath_;
  std::string expectedSha256_;
  std::string savedPath_;
  std::string errorMessage_;
  // Why the last `hello` or `pair` was refused. Not errorMessage_: a refused
  // hello is not a failed session -- see setAuthError().
  std::string authErrorMessage_;
  std::string deviceId_;
  std::string deviceNonce_;
  std::string trustedHostName_;

  TransferKind transferKind_ = TransferKind::NONE;
  size_t expectedSize_ = 0;
  size_t receivedBytes_ = 0;
  size_t sentBytes_ = 0;
  size_t lastProgressStatusBytes_ = 0;
  size_t lastDisplayProgressBytes_ = 0;
  // Outcome of the last `progress` batch. Deliberately not cleared by
  // resetTransfer(): the client reads the summary from the status published when
  // the batch finished, then issues a `start_get` for the per-entry document,
  // and that start_get resets the transfer state.
  uint32_t progressEntries_ = 0;
  uint32_t progressApplied_ = 0;
  size_t uploadChunkSize_ = 0;
  size_t uploadAckBytes_ = 0;
  size_t downloadChunkSize_ = 0;
  uint32_t expectedSequence_ = 0;
  // Next sequence to send, and the first one not yet acknowledged; frames in
  // [downloadUnacked_, downloadSequence_) are in flight.
  uint32_t downloadSequence_ = 0;
  uint32_t downloadUnacked_ = 0;
  // Frames that may be in flight at once (start_get `window`, 1..16).
  uint8_t downloadWindow_ = 1;
  // Every byte of the download has been read into a frame.
  bool downloadEof_ = false;
  // Bytes of downloadFrame_ built but not yet accepted by the stack; 0 when none.
  // A frame the stack refused is resent as-is on the next tick.
  size_t downloadFrameLength_ = 0;
  // One frame: 4-byte sequence + BLE_DOWNLOAD_CHUNK_BYTES_MAX (490). A member,
  // not a stack array (over the 256-byte local rule) and not a per-transfer heap
  // block (a sync runs several downloads back to back); BleLink is a singleton, so
  // this is 494 bytes of BSS once.
  std::array<uint8_t, 494> downloadFrame_ = {};
  // Upload bytes held before they go to the card in one multi-sector write.
  // Allocated at start_put, freed by resetTransfer().
  std::unique_ptr<uint8_t[]> uploadBuffer_;
  size_t uploadBufferCapacity_ = 0;
  size_t uploadBufferUsed_ = 0;
  // Last MTU the peer negotiated, written from the NimBLE host task and read
  // from the activity loop. 0 until an exchange happens; see notifyCapBytes().
  std::atomic<uint16_t> negotiatedMtu_{0};
  bool helloAccepted_ = false;
  bool transferOpen_ = false;
  bool downloadOpen_ = false;
  // A `pair` succeeded on this connection.
  bool hostPaired_ = false;
  bool pendingCommit_ = false;
  bool statusDirty_ = true;

  // --- transfer measurements (`about` link and last_upload) -----------------
  // LL data length before any Data Length Change event (Core Vol 6 Part B 4.5.10).
  static constexpr uint16_t LL_DEFAULT_OCTETS = 27;
  static constexpr uint16_t LL_DEFAULT_TIME_US = 328;
  struct LinkParams {
    bool valid = false;
    // A Data Length Change event arrived; until then the octets are the LL default.
    bool dataLengthReported = false;
    uint16_t intervalUnits = 0;  ///< 1.25 ms units
    uint16_t latency = 0;
    uint16_t timeoutUnits = 0;  ///< 10 ms units
    uint8_t txPhy = 0;          ///< BLE_GAP_LE_PHY_1M / _2M / _CODED; 0 until known
    uint8_t rxPhy = 0;
    uint16_t txOctets = LL_DEFAULT_OCTETS;
    uint16_t txTimeUs = LL_DEFAULT_TIME_US;
    uint16_t rxOctets = LL_DEFAULT_OCTETS;
    uint16_t rxTimeUs = LL_DEFAULT_TIME_US;
  };
  // Written by the host task, read by the main loop; both under eventMutex_.
  LinkParams linkParams_;
  void logLinkParams(const LinkParams& params, const char* cause) const;

  // Data frames of the upload in progress as they arrive, counted in the data
  // write callback under eventMutex_.
  struct UploadArrivals {
    bool active = false;
    uint32_t frames = 0;
    unsigned long firstMs = 0;
    unsigned long lastMs = 0;
    unsigned long maxGapMs = 0;
    unsigned long bucketStartMs = 0;  ///< start of the current one-second bucket
    uint32_t bucketFrames = 0;
    uint32_t bucketMaxFrames = 0;
    int minMsysFree = -1;  ///< -1 until the first frame
    size_t maxQueue = 0;
  };
  UploadArrivals uploadArrivals_;
  // The same upload as the main loop handles it. Main loop only.
  struct UploadLoopStats {
    unsigned long startMs = 0;
    uint64_t dataUs = 0;  ///< in onDataWrite, SD writes included
    uint64_t sdUs = 0;    ///< in flushUploadBuffer, plus the commit flush and close
    unsigned long sdMaxUs = 0;
    unsigned long lastTickMs = 0;
    unsigned long maxTickGapMs = 0;
    // The frame that crossed an ack boundary and is not yet notified: when the
    // host task queued it and when onDataWrite took it off the queue.
    bool ackPending = false;
    unsigned long ackArrivalMs = 0;
    unsigned long ackTakenMs = 0;
    uint32_t acks = 0;
    uint64_t ackSumMs = 0;
    unsigned long ackMaxMs = 0;
    unsigned long ackQueueMaxMs = 0;
    uint32_t notifyFailed = 0;  ///< the ack's status notify did not go out
    uint32_t ackShed = 0;       ///< it went out without `received`
    uint32_t renderCountAtStart = 0;
    uint32_t renderMsAtStart = 0;
  };
  UploadLoopStats uploadLoop_;
  // The last book, bmp or firmware upload, reported as `last_upload` in `about`.
  struct LastUpload {
    bool valid = false;
    TransferKind kind = TransferKind::NONE;
    size_t bytes = 0;
    unsigned long ms = 0;
    uint32_t frames = 0;
    int minMsysFree = -1;
    int minAclFree = -1;
    size_t maxQueue = 0;
    unsigned long sdMs = 0;
    unsigned long sdMaxMs = 0;
    unsigned long loopMs = 0;
    unsigned long maxGapMs = 0;
    unsigned long maxTickGapMs = 0;
    uint32_t framesPerSecMax = 0;
    uint32_t framesPerSecAvg = 0;
    uint32_t acks = 0;
    unsigned long ackAvgMs = 0;
    unsigned long ackMaxMs = 0;
    unsigned long ackQueueMaxMs = 0;
    uint32_t notifyFailed = 0;
    uint32_t ackShed = 0;
    uint32_t renders = 0;
    unsigned long renderMs = 0;
  };
  LastUpload lastUpload_;
  void beginUploadStats();
  // `dataSdUs`: the part of uploadLoop_.sdUs spent inside onDataWrite.
  void finishUploadStats(uint64_t dataSdUs);
  void noteAckPublished(bool notified, const std::string& notifyJson);

  // Cached answers to "what is this reader holding, and where is it?", so the
  // status can carry them without doing the work on every publish. The library
  // fingerprint is a directory walk (tens of ms for hundreds of books) and is
  // recomputed only when something says the card changed.
  uint32_t pingLibBooks_ = 0;
  uint32_t pingLibHash_ = 0;
  std::string pingBook_;
  float pingPct_ = -1.0f;
  // A book is on screen right now -- not merely the last one opened (pingBook_).
  bool pingOpen_ = false;
  // A delete_book with close:true whose book was open: the reader is being sent
  // Home, and tick() deletes once it has actually left the book.
  std::string pendingDeleteName_;
  std::string pendingDeletePath_;
  unsigned long pendingDeleteAt_ = 0;
  void deleteBookNow(const std::string& name, const std::string& path);
  bool pingLibDirty_ = true;
  // Reported to the app so it can tell "gone to sleep" from "went away".
  bool sleeping_ = false;
  // start_put asked to overwrite an existing book (a Calibre update). Reset per transfer.
  bool replaceExisting_ = false;
  // The `position` a book start_put carried, applied at commit. Reset per transfer.
  bool positionGiven_ = false;
  bool positionApplied_ = false;
  std::string positionLocation_;
  // The `calibre_uuid` a book start_put carried, stored in the sidecar at commit.
  // Reset per transfer.
  std::string calibreUuid_;
  uint32_t positionTimestamp_ = 0;
  uint16_t positionPercentBp_ = 0;
  BookProgressSync::SpineJump positionJump_;
  unsigned long lastHeartbeatMs_ = 0;
  // 60s while awake and connected. A safety net under the event triggers above,
  // not the primary mechanism: an event-driven notify is both faster and cheaper
  // than any poll, and while the reader sleeps no cadence runs at all.
  static constexpr unsigned long HEARTBEAT_INTERVAL_MS = 60UL * 1000UL;

  // Refreshes the cached ping fields. `withLibrary` pays for the directory walk.
  void refreshPingSnapshot(bool withLibrary);
  bool removePartOnExit_ = false;
  bool uploadResumable_ = false;
  bool shaActive_ = false;
  mbedtls_sha256_context shaContext_;

  void enqueueBleEvent(BleEvent event);
  void processBleEvents();
  // Writes the held upload bytes to uploadFile_. False (buffer dropped) on a short write.
  bool flushUploadBuffer();
  void onBleConnected(uint16_t connHandle);
  void onBleDisconnected(uint16_t connHandle);
  void onSecurityResult(const BleEvent& event);
  // A new bond inside the window: it becomes the only bond.
  void adoptNewBond(const std::string& peerIdAddress);
  // Enforces the hello and secure-link timeouts and ends a finished lockout.
  void checkConnectionDeadlines();
  void disconnectPeer(const char* reason);
  void onControlWrite(const std::string& value);
  // `arrivalMs`: when the host task queued the frame.
  void onDataWrite(const std::string& value, unsigned long arrivalMs);
  void processCommit();
  void startFileDownload(const char* path, const char* name, TransferKind kind, size_t offset, size_t chunkSize);
  void startCrashReportDownload(size_t offset, size_t chunkSize);
  void startLibraryDownload(size_t offset, size_t chunkSize);
  void startProgressResultDownload(size_t offset, size_t chunkSize);
  // Serialises the live settings to a scratch file and streams that, rather
  // than holding the document in RAM for the length of a chunked transfer.
  void startSettingsDownload(size_t offset, size_t chunkSize);
  void startAboutDownload(size_t offset, size_t chunkSize);
  // One EPUB from /Books by bare name, read-only (allowed while it is open).
  void startBookDownload(const std::string& name, size_t offset, size_t chunkSize);
  // Parses a committed settings document and applies it. Returns false with
  // the error already set when the document is unusable.
  bool applySettingsDocument();
  // Unpacks a committed book_meta container into the persistent sidecar store:
  // one BMP and one JSON per book, keyed by the book's filename.
  bool applyBookMetaDocument();
  // The req id the app stamped into the book_meta container. parseContainer
  // checks it, so it has to survive from start_put to commit.
  uint32_t bookMetaReq_ = 0;
  void processProgressBatch();
  void pumpDownload();
  void resetTransfer(bool removePart);
  void setState(State state);
  void setError(const std::string& error);
  // notifyStore=false refuses one request without failing the Store screen --
  // used for an answer to a question the device is no longer asking, which is a
  // normal race, not an error the user should see.
  void setError(const std::string& error, bool notifyStore);
  // A refused `hello` or `pair`. Never State::ERROR: the link stays up.
  void setAuthError(const std::string& error);
  // A refused `hello`; the third on one connection disconnects it.
  void refuseHello(const std::string& error);
  // Tell whichever screen is up that something changed. Does nothing when the
  // link is running behind a reader page, which is the normal case.
  void notifyObserver();
  void publishStatus();
  // Last authentication state the header was repainted for. The indicator is
  // drawn by every screen's header, but only ONE screen at a time can be a
  // link Observer -- so a screen that is not the observer (the home screen,
  // normally) never learns the link came up, and shows no BLE until something
  // else happens to redraw it.
  bool lastPublishedAuth_ = false;
  // The READ value, shed until it fits the 512-byte ATT attribute ceiling.
  // Never returns a document that would be served truncated.
  std::string buildReadJson() const;
  // The largest NOTIFY document that fits `capBytes`, shrinking a level at a
  // time. Never returns truncated JSON. Returns an empty string when not even
  // `{"state":"..."}` fits, meaning "send no notification at all" -- an empty
  // object parses as a status and reports as an unreadable one.
  std::string buildNotifyJson(size_t capBytes) const;
  // Appends the download chunk in use to a chosen status document, if it fits `capBytes`.
  void appendDownloadChunkSize(std::string& json, size_t capBytes) const;
};

#define BLE_LINK BleLink::getInstance()

#endif  // FREEINK_CAP_BLE_TRANSFER
