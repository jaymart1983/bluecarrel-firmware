# Security v2 (BLE protocol_version 2)

This is the contract the reader firmware and the companion app both implement. It replaces the v1 pairing
(a six-digit code sent in cleartext JSON, an unencrypted link, and a reader that never proved itself) and
the v1 firmware update check (a SHA-256 supplied by the sender).

## Threat model

| Attacker | v1 problem | v2 answer |
|---|---|---|
| Nearby BLE device posing as the reader | App trusted a status field; connected to the first advertiser | Bonded link + app pins the reader's address + reader must prove the shared secret |
| Nearby device guessing the pairing code | No attempt limit; code accepted while already paired; success replaced the phone | Pairing only inside a window the user opens on the reader; fresh passkey per attempt; attempt limit |
| Passive sniffer during pairing | `pair_secret` crossed the air in cleartext | LE Secure Connections (ECDH) with passkey MITM protection; everything after pairing is encrypted |
| Paired phone, or anyone who can write the SD card | Could enable silent auto-install and stage arbitrary firmware | Firmware must be signed by the project key; BLE cannot change auto-install; version must increase |
| Swapped image after "Later" | Later was not tied to the approved image | Deferral bound to the approved digest |
| Card access (USB Drive / removed card) | Host secret on SD, XOR with the public MAC | Host secret in NVS (internal flash) |

Out of scope: an attacker with USB access who reflashes with `esptool` (there is no secure boot; the owner must be
able to flash their own builds), and an attacker who dumps internal flash over USB.

## 1. Link security (firmware)

- NimBLE security: `bonding = true`, `mitm = true`, `sc = true` (LE Secure Connections). IO capability
  **DISPLAY_ONLY**. Pairing method is **passkey entry**: the reader shows a six-digit passkey, the phone's system
  pairing dialog asks the user to type it.
- The passkey is generated per pairing attempt with `esp_random()` while the radio is running (true RNG), in
  `onPassKeyDisplay()`, and shown on the Settings screen (`BlePairingActivity`).
- Characteristic properties: control and data-in `WRITE_ENC | WRITE_AUTHEN` (and the no-response variants where
  used); status `READ_ENC | READ_AUTHEN | NOTIFY`; data-out `NOTIFY` (plus `READ_ENC | READ_AUTHEN` if readable).
- Defence in depth: every control op, data frame and notification subscription is ignored unless the connection is
  encrypted, authenticated (MITM) and bonded (`NimBLEConnInfo::isEncrypted() && isAuthenticated() && isBonded()`).
- **Pairing window.** A new bond is accepted only while `BlePairingActivity` is on screen and either no host is paired
  or the user tapped **Pair new phone** on that screen. `onAuthenticationComplete()` for a peer that was not already
  bonded while the window is closed: delete that bond and disconnect.
- **Pair prompt.** A `pair` on an already bonded link with the window closed is never accepted on its own, because any
  app on the bonded phone can send it. `BlePairPromptActivity` asks "Pair with <host_name>?" over whatever is on
  screen; only Allow stores the held request (host id, name, secret), which is kept in RAM for at most 60 s and wiped
  on any answer. Deny, timeout, or leaving the prompt refuses it (`pairing denied`). At most one prompt per 30 s, none
  during a lockout; the answer authenticates only the connection that sent the request.
- **Attempt limit.** Three failed pairing attempts within one window close the window for 60 s (the screen says so).
  Every attempt gets a new passkey.
- **One bond.** When a new pairing succeeds, delete every other bond. The trusted-host record stays until `pair`
  replaces it; **Forget** clears the host record and every bond.
- **One connection.** At most one central connected (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1` or equivalent), and
  authentication state is bound to that connection's handle; events from any other handle are ignored.
- **Hello timeout.** A connection that has not completed a valid `hello` (or `pair`) within 20 s of encryption is
  disconnected. The count pauses while a pair prompt for that connection is on screen and restarts after a refusal. Three invalid `hello`s on one connection: disconnect.
- **Pre-authentication status** (encrypted but before `hello`/`pair` succeeds) carries only `state`,
  `protocol_version` (2), `device_id`, `device_nonce`, `has_trusted_host`, `pairing_window` (bool) and `auth_error`.
  Everything else (book, progress, library, errors, paths) is added only after authentication.

## 2. Host authentication (app layer, mutual)

The bond authenticates the phone's Bluetooth stack. The host secret additionally authenticates *this app* (any app on
the phone with Bluetooth permission can use a bonded link) and lets the app verify the reader.

- Trusted host record: `host_id` (UUID string), `host_name` (printable ASCII, ≤ 48 bytes), `secret` (32 random bytes,
  sent as 64 lowercase hex). Stored in **NVS** on the reader (Preferences namespace `bleauth`). On first boot of v2
  firmware, delete `/.crosspoint/ble_trusted_hosts.json` from the SD card; v1 pairings do not carry over.
- HMAC-SHA256 key: the **32 raw secret bytes** (not the ASCII hex, which v1 used).
- **pair** — accepted only on an encrypted+authenticated+bonded connection while the pairing window is open, or after
  the user allows it on the pair prompt:

  ```json
  {"op":"pair","version":2,"host_id":"…","host_name":"Pixel 9","secret":"<64 hex>"}
  ```

  The reader replaces any stored host, closes the window, and replies with an authenticated status containing
  `"paired":true` and the same `reader_proof` a hello produces (below, with `client_nonce` from the next hello — so the
  app follows `pair` with a normal `hello`). Refusal: `auth_error` = `pairing window closed` / `invalid pair request`.
- **hello**:

  ```json
  {"op":"hello","version":2,"host_id":"H","host_name":"N","client_nonce":"C","response":"R"}
  ```

  - `C`: 16 random bytes from the phone, 32 lowercase hex, new for every hello.
  - `D`: the reader's current `device_nonce` (read from status before sending).
  - `I`: the reader's `device_id`.
  - `R = HMAC(secret, "X4AUTH2|host|" + D + "|" + C + "|" + H + "|" + I)`, lowercase hex.
  - The reader verifies `R` in constant time. On success it sets authenticated for this connection, rotates
    `device_nonce`, and publishes a status with `"trusted_host":"N"` and
    `"reader_proof": HMAC(secret, "X4AUTH2|reader|" + D + "|" + C + "|" + H + "|" + I)` (using the pre-rotation `D`).
  - The app verifies `reader_proof` in constant time. **The app is authorised only after that check**; no status field
    ever sets it on its own. A reader whose `device_id` differs from the stored one is a different reader.
- The app sends `pair` only if Android reports the device `BOND_BONDED` and the link is up.
- The app stores the reader's Bluetooth address at pairing and, once paired, scans and connects **only** to that address
  (plus the service UUID filter). Unpaired, it scans by service UUID, the user picks the reader, and the app calls
  `createBond()`; the system dialog takes the passkey shown on the reader.
- A reader reporting `protocol_version` < 2 is refused with "Update the reader firmware".

## 3. Signed firmware

- ECDSA P-256 with SHA-256. Signed message (ASCII): `"X4FW1|" + version + "|" + sha256hex(image)` (lowercase hex).
  The signature is DER, carried as lowercase hex.
- Public key: DER SubjectPublicKeyInfo in `src/network/FirmwareSigningKey.h`
  (`FIRMWARE_SIGNING_PUBKEY_DER`). Official key SHA-256 fingerprint:
  `d84c1eb8fbe18700e9e56b777a3f5539c2e5f1c32f9adb99ad2fb8727bd86aa6`. A self-builder replaces the key with their own.
- Staging adds `/firmware/firmware.bin.sig` next to `.sha256` and `.version`. `start_put` with `kind:"firmware"`
  **requires** `version` (yyyyMMdd.HHmm) and `signature` (hex, ≤ 256 chars); missing either → error
  `signature required`. Clearing the stage removes `.sig`.
- **Ready means verified.** `FirmwareWatcher` reports READY only when: the file's SHA-256 matches `.sha256`; `.version`
  is present, well-formed and **greater than** the running `X4_BUILD_STAMP`; and the signature verifies over the
  message. Otherwise INVALID, with a reason shown in Settings: unsigned, bad signature, or not newer.
- **Verify again at install.** `SdFirmwareUpdateActivity` recomputes the file hash and re-checks signature and version
  immediately before it erases or writes flash, and aborts on any mismatch.
- **Later is bound to the image.** `deferToSleep()` records the approved SHA-256. Install at sleep happens only if the
  staged image still has that digest and still verifies. Any new firmware `start_put` or stage change clears the
  deferral.
- **Auto-install** remains a device setting, but a BLE `settings` upload cannot change `autoInstallFirmware`
  (denylisted in `applySettingsDocument`). Auto-install still requires a verified, signed, newer image.
- Hand-copied updates (SD card / USB Drive) need the `.sig` too.
- `firmware.json` gains `signature`: `{version, file, size, sha256, signature}`. The app refuses a manifest without it
  and passes it in `start_put`.
- Signing tools: `scripts/make_firmware_json.sh` signs with `openssl` when `FIRMWARE_SIGNING_KEY_FILE` is set and
  refuses to write a manifest without a signature unless `ALLOW_UNSIGNED=1`. The release workflow reads the key from
  the `FIRMWARE_SIGNING_KEY` Actions secret into a 0600 temp file and deletes it afterwards.
- Transition: firmware up to 20260914.0131 ignores signatures, so the first v2 image installs like any other. From then
  on only signed, newer images install over Bluetooth or from the card.

## 4. App networking

- HTTPS only. `network_security_config` sets `cleartextTrafficPermitted="false"` with no exceptions. Server URL and
  update page URL must begin with `https://` ("Use an https:// address"); a saved `http://` value is treated as not
  configured.
- Default update page: `https://github.com/jaymart1983/bluecarrel-firmware/releases/latest/download/` (GitHub redirects
  if the repository is renamed).
- Credentials (HTTP Basic, kosync headers) are attached only to requests whose scheme, host and port equal the
  configured server's.
- Response size caps: `firmware.json` 64 KB; firmware image aborts past the manifest `size` (and never above 8 MB);
  cover image 2 MB; kosync response 256 KB; OPDS feed 5 MB; book download 300 MB.
- The OPDS XML parser rejects documents with a DOCTYPE.
- `android:allowBackup="false"`. Password fields use a password keyboard with autocorrect off.

## 5. Other fixes in this release

- Firmware: widen the XTC size multiplications flagged by CodeQL before multiplying and bound-check them
  (`src/activities/reader/XtcReaderActivity.cpp:152`, `lib/Xtc/Xtc.cpp:162,356`, `lib/Xtc/Xtc/XtcParser.cpp:448,510`).
- Firmware: `validateImageFile` segment bound uses `dataLen > fileSize - pos` (no wrap).
- `scripts/ble_transfer.py` must not print secrets; it speaks v1 only and says so.

## Implementation notes

Where the firmware deliberately differs from, or adds to, the sections above:

- Build flags `MYNEWT_VAL_BLE_SM_SC_ONLY=1` (legacy pairing refused) and `MYNEWT_VAL_BLE_MAX_CONNECTIONS=1` in the X4 Pro
  envs. Bonds persist in NimBLE's NVS store; the bond slot count stays at the default 3, and a successful new pairing
  deletes every other bond.
- `data-out` is `NOTIFY` only; notifications on `status` and `data-out` go only to the bound connection once it is secure.
- A pairing window that is already open keeps its failure count; the count resets when a closed window opens or a
  pairing succeeds.
  The lockout is 60 s from the third failure, whatever the window does meanwhile.
- A connection that is not encrypted, authenticated and bonded within 90 s of connecting is disconnected (in addition
  to the 20 s hello timeout).
- `pair` carries no `reader_proof`. It authenticates the session and publishes `"paired":true`; the app must follow it
  with `hello` and verify that hello's `reader_proof`. A refused `pair` can also report `could not save the pairing`.
- The v1 `code` hello, `save_host`, and the `pair_host_id` / `pair_host_name` / `pair_secret` fields are removed.
  `scripts/ble_transfer.py` supports only v1 readers and stops on `protocol_version` >= 2.
- **Forget** deletes the host record and every bond. The phone must remove the reader from its Bluetooth settings before
  pairing again.
- `.version` is required as well as `.sig`. Settings also shows *Hash mismatch*, *No version* and *Invalid image*.
  In `start_put` the `signature` must be lowercase hex; the `.sig` file accepts either case.
- At install the flasher hashes the bytes it writes and switches the boot partition only if they equal the approved
  digest (*Image changed* otherwise). The deferral is bound to that digest and to the staging generation, so any
  restage on the device clears it.
- **Settings > System > SD Card Firmware Update** and recovery mode (Down + Power at boot on the X4 Pro) still flash any
  valid image the owner picks on the device, signed or not.
- Release tags: `x4pro-<yyyyMMdd.HHmm>` publishes a release, `x4pro-<stamp>-test` a pre-release. Assets: `.bin`,
  `.bin.sig`, `-full.bin`, `firmware.json` (with `signature`) and `SHA256SUMS`.
