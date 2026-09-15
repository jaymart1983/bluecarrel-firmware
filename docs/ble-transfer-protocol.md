# BLE Transfer Protocol

CrossPoint Reader advertises this service **whenever the device is awake**. It is not owned by any screen: it
starts at the end of boot, stops on the way into deep sleep, and comes back on wake (which is a chip reset, so
"on wake" and "at boot" are the same code path). There is no Bluetooth Transfer screen any more, and there is no
longer any screen a user has to find and keep open before a phone can reach the reader.

Advertising runs at a deliberately slow interval — 1000-1285 ms — because this link is for occasional sync, not
low latency. A scanning phone still finds the reader within a second or two.

The main client is the **Bluecarrel** Android app. `scripts/ble_transfer.py` is a command-line test client for
protocol 1 readers only.
The X4 Pro build has no Wi-Fi, so this link is its only wireless path: books, reading positions, settings and
firmware updates all travel over it.

While the link is up the reader sends a small status notification every 60 seconds (the heartbeat), and also when a
book opens or closes and just before the device goes to sleep. See [Heartbeat fields](#heartbeat-fields).

## Compatibility

- Protocol version: `2`
- Device name: the reader's `deviceName` setting, or `Bluecarrel` when it is blank (protocol 1 readers advertised
  `CrossPoint Transfer`). It is sent in the scan response, not the advertisement: the flags and the 128-bit service
  UUID take 21 of the advertisement's 31 bytes. It is also the GAP Device Name. A new name is advertised at once
  when no phone is connected, otherwise from the next advertising start. Android may keep showing a cached
  `BluetoothDevice.getName()`; read the name from the scan record.
- Service UUID: `6f9f0a00-9b1d-4d1f-9f53-5b6b8b3d0f10`

Clients should discover the service by UUID. The user-visible name is not part of the compatibility contract.

A `hello` or `pair` with any `version` other than `2` is refused. `scripts/ble_transfer.py` speaks only protocol 1 and
stops when a reader reports `protocol_version` 2 or higher. The design and threat model are in
[security-v2.md](./security-v2.md).

## Characteristics

| Name | UUID | Direction | Properties |
| --- | --- | --- | --- |
| `control` | `6f9f0a01-9b1d-4d1f-9f53-5b6b8b3d0f10` | client to reader | write with response; encrypted, authenticated link required |
| `data-in` | `6f9f0a02-9b1d-4d1f-9f53-5b6b8b3d0f10` | client to reader | write, write without response; encrypted, authenticated link required |
| `status` | `6f9f0a03-9b1d-4d1f-9f53-5b6b8b3d0f10` | reader to client | read (encrypted, authenticated link required), notify (the read and the notification carry *different* documents — see [The Store](#the-store-requests-over-the-notify-channel)) |
| `data-out` | `6f9f0a04-9b1d-4d1f-9f53-5b6b8b3d0f10` | reader to client | notify |

## Link security

- **LE Secure Connections only**, with bonding and MITM protection. Legacy pairing is refused by the stack.
- The reader's IO capability is display only, so pairing is **passkey entry**: the reader shows a six-digit passkey on
  its Settings page and the phone's system pairing dialog asks the user to type it. Every attempt gets a new passkey.
- **One connection at a time.** A second central is disconnected.
- Writes to `control` and `data-in` are ignored, and nothing is notified on `status` or `data-out`, unless the one
  connection is encrypted, authenticated and bonded.
- Bonds are kept in NimBLE's NVS store. When a new pairing succeeds, the reader deletes every other bond, so it is
  bonded to one phone. The stored host record stays until `pair` replaces it; **Forget** clears the host and every bond.

### Pairing window

A new bond is accepted only while the pairing window is open:

- It is open while the Settings page is on screen and no phone is paired, or after the user taps **Pair new phone** on
  that page.
- It closes when the user leaves the Settings page, and when a `pair` succeeds.
- With the window closed, a pairing attempt is disconnected and any new bond it produced is deleted. A phone that is
  already bonded reconnects whether or not the window is open.
- Three failed attempts (wrong passkey, unauthenticated pairing, or disconnecting while the passkey is shown) lock
  pairing for 60 s. The Settings page shows the countdown.

### Pair prompt

A phone that is still bonded can send `pair` while the window is closed, for example after the app was reinstalled
and lost its secret. Any app on a bonded phone could send it, so the reader never accepts it on its own: it asks the
person holding the reader. Support is advertised as `pair_prompt` in the `features` list of [`about`](#about).

- When a valid `pair` arrives with the window closed, no lockout running and no prompt in the last 30 s, the reader
  shows **Pair with <host_name>?** with **Allow** and **Deny** over whatever is on screen. A book that is open stays
  open underneath. The prompt does not close on an outside tap or Back.
- While it is up, `status` reports `auth_error: "confirm on reader"`. The 20 s `hello` deadline is paused.
- **Allow** applies the held request exactly as if the window had been open: the stored host is replaced and
  `status` reports `"paired":true` and `trusted_host`. The app does not resend `pair`; it follows with `hello` as
  usual. If the phone disconnected before Allow, the host is still stored and its next `hello` authenticates.
- **Deny**, leaving the prompt any other way, or no answer within 60 s: `auth_error: "pairing denied"`, and the
  `hello` deadline starts again.
- Only one prompt starts per 30 s. A `pair` during that time, or while a prompt is up, is refused with
  `pairing window closed`. During a pairing lockout every `pair` with the window closed gets `pairing window closed`.
- A malformed `pair` is refused with `invalid pair request` and shows no prompt.
- Firmware without `pair_prompt` refuses a closed-window `pair` with `pairing window closed`.

### Timeouts and limits

| Condition | Result |
| --- | --- |
| Link not encrypted, authenticated and bonded within 90 s of connecting | Disconnected |
| No accepted `hello` or `pair` within 20 s of the link being secured (paused while a [pair prompt](#pair-prompt) is up) | Disconnected |
| [Pair prompt](#pair-prompt) not answered within 60 s | `auth_error: "pairing denied"` |
| Three refused `hello`s on one connection | Disconnected |
| Three failed pairing attempts | Pairing locked for 60 s |

### Status before authentication

Until a `hello` or `pair` is accepted on the connection, `status` carries only:

| Field | Type | Meaning |
| --- | --- | --- |
| `state` | string | Link state |
| `protocol_version` | number | `2` |
| `device_id` | string | Reader identity used in the `hello` HMAC. GATT read only. |
| `device_nonce` | string | Reader nonce for the next `hello`. GATT read only. |
| `has_trusted_host` | bool | A host record is stored |
| `pairing_window` | bool | The pairing window is open and not locked |
| `auth_error` | string | Why the last `hello` or `pair` was refused. Absent when none. |

A notification that does not fit drops `protocol_version` first, then `has_trusted_host` and `pairing_window`, then
`auth_error`. Book, progress, library, heartbeat and error fields are sent only after authentication.

## Authentication

The bond authenticates the phone's Bluetooth stack. On top of it the app and the reader share a 32-byte secret, which
authenticates the app to the reader and the reader to the app.

The reader stores one host record in NVS (Preferences namespace `bleauth`): `host_id`, `host_name` and the secret. The
v1 file `/.crosspoint/ble_trusted_hosts.json` is deleted from the SD card; v1 pairings do not carry over.

Field rules used below:

- `host_id`: 1-64 characters, letters, digits, `-` and `_`.
- `host_name`: cut to 48 bytes; any character outside printable ASCII becomes `?`; empty or missing is stored as
  `Trusted host`.
- HMAC key: the **32 raw secret bytes**, not their hex text.

### `pair`

Sent once, on a freshly bonded link, while the pairing window is open. With the window closed the reader asks the
person holding it first; see [Pair prompt](#pair-prompt).

```json
{"op":"pair","version":2,"host_id":"H","host_name":"Pixel 9","secret":"<64 lowercase hex>"}
```

`secret` is 32 random bytes as 64 lowercase hex characters. On success the reader replaces any stored host, closes the
pairing window, marks the session authenticated and publishes `status` with `"paired":true` and `trusted_host`.

`pair` carries no `reader_proof`. The app must follow it with a `hello` on the same connection and check the
`reader_proof` that hello produces before it trusts the reader.

Refusals, reported as `auth_error`: `pairing window closed`, `invalid pair request`, `could not save the pairing`,
`pairing denied`. `confirm on reader` is not a refusal: the reader is showing the pair prompt.

### `hello`

```json
{"op":"hello","version":2,"host_id":"H","host_name":"N","client_nonce":"C","response":"R"}
```

- `D`: the reader's `device_nonce`, from a GATT read of `status` just before the hello.
- `I`: the reader's `device_id`, from the same read.
- `C`: 16 random bytes from the phone as 32 lowercase hex characters, new for every hello.
- `H`: the `host_id` sent at pairing.
- `R`: HMAC-SHA256 as 64 lowercase hex characters over

  ```text
  X4AUTH2|host|{D}|{C}|{H}|{I}
  ```

The reader compares `R` in constant time. On success it marks the session authenticated, rotates `device_nonce`, and
publishes `status` with `trusted_host` and `reader_proof`, the HMAC-SHA256 as lowercase hex over

```text
X4AUTH2|reader|{D}|{C}|{H}|{I}
```

using the same `D` the hello was computed over. The app must verify `reader_proof` in constant time; no other status
field makes a reader trusted.

Send `host_name` on every hello. When it differs from the stored name, the reader stores the new one.

`device_nonce` also changes on every disconnect, so read it again after reconnecting.

### Refusals

A refused `hello` or `pair` is **not** a failed session and never enters `state: "error"`. The link stays up, any
earlier authentication on the connection is cleared, and the reason is reported in `status` as `auth_error`, cleared by
the next accepted hello. Three refused hellos on one connection disconnect it. Any other op sent before authentication
is refused with `auth_error: "hello required"`.

| `auth_error` | Meaning |
| --- | --- |
| `unsupported protocol version` | `version` is not `2` |
| `invalid hello` | A field is missing or malformed |
| `unknown trusted host` | With `has_trusted_host: false`, nobody is paired. With `true`, a different host is paired. |
| `invalid trusted host auth` | `response` did not verify |
| `pairing window closed` | `pair` sent while the window is closed and no pair prompt can start (lockout, or another prompt in the last 30 s) |
| `invalid pair request` | A `pair` field is missing or malformed |
| `confirm on reader` | Not a refusal. The reader is asking the user to allow a closed-window `pair`. |
| `pairing denied` | The user chose Deny, or the pair prompt went unanswered for 60 s |

### Forgetting

The user forgets the phone with **Forget** on the Settings page. That deletes the host record and every bond. There is
no protocol operation for it. To pair again, the phone must first remove the reader from its own Bluetooth settings.

## Supported Operations

Uploads use `start_put`, binary frames on `data-in`, then `commit`.

`start_put` fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `kind` | string | Upload kind, below. Required. |
| `name` | string | File name for `book` (`.epub`), `bmp` (`.bmp`) and `firmware` (`.bin`). No path separators or traversal. |
| `size` | number | Total bytes. Required. |
| `sha256` | string | 64 hex characters over the whole payload. Required. |
| `replace` | bool | `book` only. `true` overwrites an existing `/Books/<name>`; without it an existing file is refused as `exists`. Replacing the book that is open on screen is refused as `book open`. Default `false`. |
| `position` | object | `book` only, optional. The reading position to open the book at, applied on commit before the book appears on the shelf. See [Opening position](#opening-position). |
| `calibre_uuid` | string | `book` only, optional. The Calibre book UUID, 1-64 characters of `0-9`, `A-Z`, `a-z` and `-`. Anything else fails `start_put` with `invalid calibre_uuid`. Stored in the book's metadata sidecar on commit (created when there is none; title, author and the other fields are kept) and reported by `library`. Advertised as `book_uuid`. |
| `version` | string | `firmware` only, required. The image's build stamp, `yyyyMMdd.HHmm` (for example `20260913.1914`). Saved as `firmware.bin.version`. |
| `signature` | string | `firmware` only, required. The image signature as lowercase hex, even length, at most 256 characters (see [Signatures](#signatures)). Saved as `firmware.bin.sig`. |
| `req` | number | Only when answering a Store request. |
| `resume`, `chunk_size`, `ack_bytes` | bool, number, number | Resumable upload and credit flow-control options. |

Supported upload kinds:

- `book`: `.epub` saved under `/Books`
- `bmp`: `.bmp` saved under `/Pictures`
- `firmware`: `.bin` **dropped into the watched folder** — see [Firmware updates](#firmware-updates). Requires
  `version` and `signature`; without a well-formed pair `start_put` fails with `signature required`. It is validated on
  commit and then left there; nothing is flashed during the session
- `progress`: a batch of reading positions to apply to books already on the card (see below)
- `settings`: a settings document to apply. Refused as `book open` while a book is open. `deviceName` is trimmed and
  must then be at most 16 bytes of printable ASCII (`0x20`-`0x7E`); empty means `Bluecarrel`. A name that breaks the
  rule is ignored and the old name kept; the rest of the document still applies
- `book_meta`: book metadata for the app's library, capped at a small size. The entry may carry `calibre_uuid`
  (same rule as on `start_put`; otherwise `invalid calibre_uuid`). Without one, a `calibre_uuid` already in the
  sidecar is kept
- `catalog_page`: one screen of the app's Calibre library, answering a `catalog_page` request (see
  [The Store](#the-store-requests-over-the-notify-channel))
- `catalog_detail`: one book in full, answering a `catalog_detail` request

Downloads use `start_get`, notifications on `data-out`, and `get_ack` from the client. See
[Download frames and acknowledgement](#download-frames-and-acknowledgement).

Supported download kinds:

- `crash_report`: reads `/crash_report.txt`
- `book`: one EPUB from `/Books` (see [`book` download](#book-download))
- `library`: the on-device book list with reading progress (see below)
- `progress_result`: the per-entry outcome of the last `progress` upload (see below)
- `settings`: the device's current settings, serialised fresh on each request at `offset: 0`
- `about`: which firmware is running and whether an update is staged (see [`about`](#about))

The other control ops are `set_time` (see [Device clock](#device-clock)), `delete_book` (see
[`delete_book`](#delete_book)), `set_dark_mode` (see [`set_dark_mode`](#set_dark_mode)), `catalog_error` (the app
declining a Store request it cannot answer) and `cancel`.
`hello` and `pair` are described under [Authentication](#authentication).

## Download frames and acknowledgement

```json
{"op":"start_get","kind":"library","offset":0,"chunk_size":490,"window":8}
```

`start_get` fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `kind` | string | Download kind, above. Required. |
| `offset` | integer | Byte offset to start from. Default `0`. Must be a multiple of the chunk size in use unless it equals the document size; otherwise `unaligned download offset`. |
| `chunk_size` | integer | Payload bytes per frame. Default `160`. Accepted from `20` up to `download_chunk_max` from the [`about`](#about) download (`490`); older firmware accepts at most `160`. Outside that range: `invalid download chunk size`. |
| `window` | integer | Frames the reader may send past the last acknowledged one, `1`–`16`. Default `1`. Anything else, including a non-integer: `invalid window`. Older firmware ignores it and behaves as `1`. |

**Chunk size.** A `chunk_size` of `160` or less is used as asked. A larger one is shrunk to what one notification on
the current link can carry, `ATT_MTU - 3 - 4`, but never below `160`; it is not refused. At the MTU of 517 the reader
asks for, `490` is used as asked; at MTU 185, `178` is used. The chunk in use is reported as `chunk_size` in a
`sending` status (notification or GATT read) whenever it fits beside `sent` without dropping any other field. A client
that does not see it can rely on frame lengths: every frame but the last carries exactly one chunk.

`490` is the default maximum because a frame costs `chunk + 11` bytes on the link (4-byte sequence, 3-byte ATT
header, 4-byte L2CAP header): with the 251-byte LL data length the reader requests, 491 is the largest chunk that fits
in two LL PDUs, where the 510-byte MTU ceiling needs three.

**Frames.** Each frame is one notification on `data-out`: a little-endian `uint32` sequence number followed by the
payload. The first sequence number is `offset / chunk_size`.

**Acknowledgement.** `{"op":"get_ack","sequence":N}` is cumulative: it acknowledges every frame up to and including
`N`. The reader sends up to `window` frames past the last acknowledged sequence and then waits. With `window: 1` that
is one frame per `get_ack`, which is how older firmware always works. A client using a window acknowledges every
`window`-th frame and always the final frame. `N` must name a frame that was sent and not yet acknowledged; otherwise
the reader reports `unexpected download ack` (or `no download pending` when nothing is waiting for an ack).

`state: "sent"` is published only after the final frame has been acknowledged. While sending, `status` carries `sent`
and `size`, and `chunk_size` as described above.

The reader also paces itself: it holds back frames while NimBLE's buffers run low, so a window is a maximum, not a
promise that `window` frames arrive back to back. The client must not treat a pause shorter than its stall timeout as
an error.

## Opening position

A `book` upload may carry the position the reader should open it at, so a book sent with "resume at 89.5%" opens
there the first time, not at 0%:

```json
{"op":"start_put","kind":"book","name":"Dune.epub","size":…,"sha256":…,
 "position":{"timestamp":1725600000,"pct":0.895,"spine":41,"spine_n":58,"spine_frac":0.37}}
```

`position` takes the same fields as one [`progress`](#progress) entry, without `filename` (the book is `name`):

| Field | Type | Meaning |
| --- | --- | --- |
| `timestamp` | integer | UTC epoch seconds the position was reached. Required, and inside the range `set_time` accepts. |
| `location` | string | Optional. Hex-encoded saved position (see [Position encoding](#position-encoding)); a length the EPUB reader writes (8, 12 or 20 characters). |
| `spine`, `spine_n` | integer | Optional, both or neither. Spine item index and the spine item count the sender measured; `0 <= spine < spine_n <= 65535`. |
| `spine_frac` | number | Optional, with `spine`. Fraction `0`–`1` through that spine item. Default `0`. |
| `pct` | number | Optional. Progress through the book, `0`–`1`, for the library screen. |

At least one of `location` or `spine`/`spine_n` is required. It is checked at `start_put`: a `position` that is not an
object, or has a missing or out-of-range field, a field of the wrong type, `spine_frac` without `spine`, or a
malformed `location`, refuses the whole `start_put` with `error: "invalid position"`. An empty `location` string
counts as absent.

On `commit`, once the book is at its final path, the position is applied with the [conflict rule](#conflict-rule)
**before** the Home shelf and library are told the book exists, so nothing can open it first. A new book has no saved
position, so the position applies. A `replace` keeps the book's existing saved position, and the incoming one applies
only if its `timestamp` is newer. The upload succeeds either way, and the committed status reports the outcome:

```json
{"state":"saved","kind":"book","received":1048576,"ack_bytes":4096,"size":1048576,"position_applied":true,…}
```

`position_applied` is present only when the `start_put` carried `position`. `false` means the position was not written
(for example the device already had a newer save). The firmware log gives the result name. On `false` the app can
send a `progress` batch as usual. It is dropped from a tight notification with the transfer counters, so read
`status` if it is missing. Firmware that does not support this ignores `position` and never reports `position_applied`.
It advertises support as `book_position` in the `features` list of the [`about`](#about) download.

## `delete_book`

```json
{"op":"delete_book","name":"Dune.epub","close":true}
```

Deletes one book from `/Books`. `name` follows the `book` upload rule: a bare `.epub` file name, no folders, no
wildcards. Like every other op it requires `hello` first.

- On success `status` reports `state: "saved"`. A book that is already absent also reports `saved`.
- The book's cache is cleared and the Home shelf is refreshed.
- Only the book that is **open on screen** blocks its own deletion. Without `close` (default `false`) that case is
  refused with `error: "book open"`. With `close: true` the reader leaves the book (saving its position) and returns
  to Home, then deletes the file. If the reader has not closed within about five seconds the request fails with
  `book open`.
- Other errors: `unsafe book filename`, `could not delete the book`.

## `book` download

```json
{"op":"start_get","kind":"book","name":"Dune.epub","offset":0,"chunk_size":490,"window":8}
```

Sends `/Books/<name>` byte for byte through the ordinary frame/ack path, so `offset`, `chunk_size` and `window`
behave as for every other download and an interrupted download resumes at an aligned `offset`. `status` reports
`kind: "book"`, `size`, `sent`, and `name` once `state` is `sent`. Like every other op it requires `hello` first.

- `name` follows the `delete_book` rule: a bare `.epub` file name, no folders.
- It works while that book is open on screen; the file is only read.
- A `delete_book` for the same book stops the download before deleting.
- Errors: `unsafe book filename`, `not found`, plus the download errors every kind has.

Support is advertised as `book_download` in the `features` list of the [`about`](#about) download, and `book` is in
`download_kinds`.

## `set_dark_mode`

```json
{"op":"set_dark_mode","dark":true}
```

Switches the reader between dark mode (inverted output) and normal output. Like every other op it requires `hello`
first. It works with a book open.

- `dark` is required and must be a JSON boolean; anything else fails with `error: "invalid dark_mode"`.
- If the reader is already in the requested mode, nothing happens: no save, no repaint.
- Otherwise the setting is saved and whatever is on screen, including an open book, is redrawn once with a clean full
  refresh so no ghost of the old polarity remains.
- Success does not change `state` and adds nothing to `status`. Read the result from `dark_mode` in the
  [`about`](#about) download.

Support is advertised as `dark_mode` in the `features` list of the [`about`](#about) download.

## `about`

`{"op":"start_get","kind":"about"}` returns one small JSON object:

```json
{"firmware_version":"20260913.1914","running_partition":"app1","update_staged":true,
 "install_at_sleep":true,"staged_version":"20260914.0800","download_chunk_max":490,"dark_mode":false,
 "device_name":"Bluecarrel",
 "link":{"interval_ms":15,"latency":0,"timeout_ms":4000,"tx_octets":251,"rx_octets":251,"dl_reported":true,"phy":"2M"},
 "last_upload":{"kind":"firmware","bytes":4677152,"ms":135800,"frames":9430,"min_msys_free":9,"min_acl_free":6,
  "max_queue":12,"sd_ms":3100,"sd_max_ms":40,"loop_ms":900,"max_gap_ms":180,"frames_per_s_max":70,
  "frames_per_s_avg":68,"tick_gap_max_ms":45,"acks":197,"ack_notify_avg_ms":12,"ack_notify_max_ms":60,
  "ack_queue_max_ms":20,"notify_failed":0,"ack_shed":0,"renders":5,"render_ms":1200},
 "features":["book_position","download_window","dark_mode","book_uuid","book_download","pair_prompt"]}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `firmware_version` | string | Build stamp of the running firmware, `yyyyMMdd.HHmm` (UTC). Stamps sort in build order as strings. |
| `running_partition` | string | OTA slot the reader booted from (`app0` or `app1`). After a Bluetooth update the reader may be running `app1`. |
| `update_staged` | bool | `/firmware/firmware.bin` and its `.sha256` file are both on the card. |
| `install_at_sleep` | bool | A staged image will be installed the next time the reader sleeps (the user chose Later, or auto-install is on). |
| `staged_version` | string | Contents of `/firmware/firmware.bin.version`. Absent when there is no such file. |
| `download_chunk_max` | integer | The largest `start_get` `chunk_size` this firmware accepts (`490`). Absent on older firmware, which accepts at most `160`. See [Download frames and acknowledgement](#download-frames-and-acknowledgement). |
| `dark_mode` | bool | `true` while the reader draws inverted (dark mode). Set with [`set_dark_mode`](#set_dark_mode). Absent on older firmware. |
| `device_name` | string | The name the reader advertises: its `deviceName` setting, or `Bluecarrel` when that is blank. Absent on older firmware. |
| `link` | object | The connection as it is now; absent when no phone is connected. See [`link`](#link-and-last_upload). |
| `last_upload` | object | Measurements of the last `book`, `bmp` or `firmware` upload since boot. Absent before one. See [`last_upload`](#link-and-last_upload). |
| `features` | array of strings | Protocol features beyond the upload and download kinds. `book_position`: a `book` upload accepts `position` (see [Opening position](#opening-position)). `download_window`: `start_get` accepts `window` and `get_ack` is cumulative (see [Download frames and acknowledgement](#download-frames-and-acknowledgement)). `dark_mode`: the `set_dark_mode` op is supported and `dark_mode` is reported here. `book_uuid`: `start_put` for `book` and `book_meta` accept `calibre_uuid`, and `library` reports it. `book_download`: the `book` download kind (see [`book` download](#book-download)). `pair_prompt`: a closed-window `pair` asks on the reader (see [Pair prompt](#pair-prompt)). Absent on older firmware. |

`about` is not listed in `download_kinds`.

### `link` and `last_upload`

Diagnostics for transfer speed. Both are measurements, not protocol: a client must not change what it sends because
of them. Older firmware omits both objects.

`link` is recorded on connect and on every connection update, PHY update and LL data length change. The reader also
logs each change at INFO (`link connected: ...`, `link updated: ...`, `link phy: ...`, `link data length: ...`).

| Field | Type | Meaning |
| --- | --- | --- |
| `interval_ms` | number | Connection interval in ms (1.25 ms units, so `7.5`, `11.25`, `15` ...). |
| `latency` | integer | Peripheral latency, in connection events. |
| `timeout_ms` | integer | Supervision timeout in ms. |
| `tx_octets`, `rx_octets` | integer | LL data length in force, reader to phone and phone to reader. |
| `dl_reported` | bool | `false` until the controller reports an LE Data Length Change; `tx_octets`/`rx_octets` are then the 27-octet LL default. |
| `phy` | string | `1M`, `2M` or `coded`; `tx/rx` (e.g. `2M/1M`) when the directions differ. |

`last_upload` covers one upload from `start_put` to `commit`. The reader logs the same numbers at INFO in two lines
(`upload <kind>: ...` and `upload loop: ...`) when the commit starts.

| Field | Type | Meaning |
| --- | --- | --- |
| `kind` | string | `book`, `bmp` or `firmware`. |
| `bytes` | integer | Payload bytes received. |
| `ms` | integer | `start_put` to `commit`. |
| `frames` | integer | Data frames that arrived. |
| `min_msys_free` | integer | Fewest free NimBLE msys blocks, sampled in the data write callback. |
| `min_acl_free` | integer | Fewest free blocks in the host transport's controller-to-host ACL pool (`transport_pool_acl`). At `0` the transport stalls 10 ms per retry. Absent when the pool is not found. |
| `max_queue` | integer | Deepest the reader's BLE event queue got (limit 192 events / 32 KB). |
| `sd_ms` | integer | Main loop time writing the upload to the card: buffered writes plus the commit flush and close. |
| `sd_max_ms` | integer | Longest single buffered write. |
| `loop_ms` | integer | Main loop time handling data frames, excluding SD writes (sequence checks, SHA-256, copying). |
| `max_gap_ms` | integer | Longest gap between two consecutive data frames arriving. |
| `frames_per_s_max` | integer | Most data frames that arrived in one one-second bucket. |
| `frames_per_s_avg` | integer | Frames per second from the first frame to the last. |
| `tick_gap_max_ms` | integer | Longest gap between two main loop link ticks during the upload. |
| `acks` | integer | Ack boundaries (every `ack_bytes`, and the last byte) whose status notification was sent. |
| `ack_notify_avg_ms`, `ack_notify_max_ms` | integer | From the frame that crossed an ack boundary arriving to the status notification carrying `received` being handed to the stack. |
| `ack_queue_max_ms` | integer | The part of that spent waiting in the event queue. |
| `notify_failed` | integer | Ack notifications the stack did not take (no buffer, or not subscribed). |
| `ack_shed` | integer | Ack notifications that went out without `received`. |
| `renders` | integer | Screen renders completed during the upload. |
| `render_ms` | integer | Their total duration. |

## Firmware updates

A firmware push is a **file drop**, not an interactive flow. The reader watches one folder:

| Path | What it is |
| --- | --- |
| `/firmware/firmware.bin` | the ESP32 application image |
| `/firmware/firmware.bin.sha256` | its SHA-256, as text |
| `/firmware/firmware.bin.version` | its build stamp, `yyyyMMdd.HHmm`, as text. Required. |
| `/firmware/firmware.bin.sig` | its signature, hex text. Required. |
| `/firmware/.firmware.bin.part` | scratch: where a BLE upload accumulates before the rename |

The `.sha256` file's **first whitespace-delimited token** is a 64-character hex SHA-256 of the image, which is the first
field of `sha256sum firmware.bin`. Case is not significant in `.sha256` or `.sig`; a trailing newline is fine.

### Signatures

- Algorithm: ECDSA P-256 with SHA-256. The signature is DER, carried as lowercase hex.
- Signed message, ASCII, no newline:

  ```text
  X4FW1|{version}|{sha256 of the image, lowercase hex}
  ```

- Public key: `FIRMWARE_SIGNING_PUBKEY_DER` in `src/network/FirmwareSigningKey.h`.
- `scripts/make_firmware_json.sh` produces the `.sig` and a `firmware.json` with a `signature` field.

### Staging

Two routes put files there:

- **Over BLE.** `start_put` with `kind: "firmware"`, `version` and `signature`, then frames and `commit`. Starting a
  `firmware` upload first removes any staged image and its companion files. On commit the reader validates the image
  (`firmware_flash::validateImageFile`: header magic, segment table, XOR checksum, SHA-256 trailer, chip id, board tag)
  and, if it passes, writes `.sha256` from the digest it just verified, plus `.version` and `.sig`. `status` reports
  `state: "saved"`; a bad image reports `state: "error"` with `error: "invalid firmware: <REASON>"`. The signature is
  checked by the watcher, not at commit.
- **Over USB.** Plug the reader into a computer and copy `firmware.bin`, `firmware.bin.sha256`,
  `firmware.bin.version` and `firmware.bin.sig` into `/firmware`, then eject.

### Checks

The reader checks the folder every 30 seconds (not during the first 20 seconds after boot) and re-hashes the image off
the card a few KB per main-loop tick. The update is offered only when all of these hold, checked in this order:

| Check | Settings shows when it fails |
| --- | --- |
| `.sha256` is usable and the image is at least 64 KB | *Invalid image* |
| The image hashes to `.sha256` | *Hash mismatch* |
| `.sig` is present | *Unsigned image* |
| `.version` is `yyyyMMdd.HHmm` | *No version* |
| The signature verifies over the message above | *Bad signature* |
| `.version` is newer than the running build stamp | *Not newer* |

A refused image is left on the card. With `.bin` or `.sha256` missing nothing is staged and Settings shows *Up to
date*.

### Install

- **Default:** it shows **Firmware update found** over whatever screen is up, a book included, with **Update Now**,
  **Later** and **Cancel**. The question stays until one is chosen, or until the user goes Home from the Control
  Centre (the update then remains available from Settings).
  - **Update Now** leaves the current screen (an open book saves its position), installs, then reboots.
  - **Later** installs at the next sleep: on the way into sleep the reader flashes, reboots, and goes straight back to
    sleep. The deferral is bound to the approved image; restaging on the reader cancels it, and a restart forgets it.
  - **Cancel** deletes nothing. The image is not offered again until the staged files change or the device restarts.
- **Auto-install** (setting `autoInstallFirmware` on): no prompt. The image installs at the next sleep, as for Later.
  A `settings` upload cannot change `autoInstallFirmware`; it is set on the device only.

Immediately before writing flash the reader hashes the image again, requires the digest the user approved, and repeats
every check above. It hashes the bytes as it writes them and switches the boot partition only if they match; otherwise
the install fails with *Image changed*. All four files are deleted after a successful install.

`install_at_sleep` and `update_staged` in the [`about`](#about) download report this state to the client.

### What is not covered

**Settings > System > SD Card Firmware Update** and firmware recovery mode (Down + Power held at boot on the X4 Pro)
flash any valid image the user picks on the device, signed or not. Flashing over USB with `esptool` always works; there
is no secure boot.

The Store also runs **the other way round** — the device asks, the app answers — over the `status` notify
channel. See [The Store](#the-store-requests-over-the-notify-channel).

### `library`

`{"op":"start_get","kind":"library"}` makes the reader walk `/Books`, stage a JSON document on the SD card, and then
send it through the ordinary frame/ack path. `status` reports `state: "preparing"` while the shelf is being walked
(seconds on a large library), then `state: "sending"` with `kind: "library"`, `size`, and `sent` as usual. Like every
other operation it is refused until `hello` has been accepted.

The payload is a JSON array, one object per book:

```json
[
  {"filename":"Dune.epub","size":1048576,"title":"Dune","author":"Frank Herbert","percent":0.4237,
   "location":"0300170023000000","timestamp":1725600000},
  {"filename":"Classics/Ulysses.epub","size":2097152,"title":"Ulysses","author":"James Joyce","percent":0}
]
```

| Field | Type | Source |
| --- | --- | --- |
| `filename` | string | Path relative to `/Books`, so a book in a sub-folder reads `Sub/Folder/Book.epub`. Always present. |
| `size` | number | Size of the book file in bytes. Always present. |
| `title` | string | The book's cached metadata title. Falls back to the bare filename when no metadata is available; never empty. |
| `author` | string | The book's cached metadata author. Empty string when unknown. |
| `percent` | number | Progress through the whole book, `0`–`1`, rounded to four decimals. `0` for a book that was never opened. **Display only** — never sync on this. Always present. |
| `location` | string | The saved position, exactly as the device stores it, hex-encoded. Absent for a book that was never opened. This is the field to sync on. |
| `timestamp` | number | UTC epoch seconds at which `location` was saved. **Absent means unknown**, not `0`. |
| `calibre_uuid` | string | The Calibre book UUID. From the book's sidecar (`calibre_uuid` on a `book` or `book_meta` upload) when it has one; otherwise from the EPUB's OPF identifier, only if the reader already indexed the book with this firmware (see below). Absent when unknown. |
| `fromApp` | bool | The book has a metadata sidecar, i.e. it arrived from the app. |
| `lastRead` | number | **Never emitted by this firmware.** Use `timestamp`. See below. |

Notes on the fields:

- Books are the formats the reader can open: `.epub`, `.xtc`/`.xtch`, `.txt`, `.md`. Sub-folders of `/Books` are walked
  to a depth of four; dot-entries are skipped. Entries are ordered per folder the way the file browser orders them.
- `title`/`author` come from the per-book metadata cache the reader writes the first time a book is opened, not from
  the recents list, so every book on the card is covered rather than only the ten most recent. A book that has never
  been opened has no EPUB metadata cached and lists under its filename with an empty author; the reader deliberately
  does not index books to answer this request. XTC books carry metadata in the file header, so they list correctly
  even before first open. `.txt`/`.md` have no embedded metadata at all and always list under their filename.
- `percent` is derived from the book's saved position, using the same mapping the reader and the KOReader sync path
  use. `.txt`/`.md` books always report `0`: their saved position is a page index whose page count depends on the
  current font, margins, and viewport, and none of that is recorded on disk, so no percentage can be recovered.
  `location` and `timestamp` are still exact for those books, which is why syncing on the stored position covers
  them and syncing on a percentage would not.
- `location` is the reader's own `progress.bin` byte for byte (see [Position encoding](#position-encoding)). Hand it
  straight back in a `progress` upload and the book reopens exactly where it was. Nothing anywhere converts a
  percentage into a position: that conversion is lossy — recovering a spine index and page from `0.4237` needs the
  book's pagination, which depends on the current font, margins and viewport — so it is not offered at all.
- `timestamp` is when that position was saved, from the device's own clock. On a board with an RTC it is effectively
  always present: the clock is started from the firmware's build epoch on first boot, so the device knows a time before
  any client has set one (see [Device clock](#device-clock)). It is omitted for a book whose position predates this
  firmware, for one whose sidecar was lost, and on a board with no RTC. Absent means unknown, and unknown loses every
  conflict.
- `calibre_uuid` from the OPF: when the reader indexes an EPUB (the first time it is opened, or after its cache was
  cleared) and the OPF has a `dc:identifier` with `opf:scheme="calibre"`, `id="calibre_id"` or `id="uuid_id"`
  (in that order of preference) whose value is a UUID (a `urn:uuid:` or `calibre:` prefix is dropped; 32 hex digits,
  hyphens optional), it is saved as `calibre_uuid.txt` in the book's cache directory. The listing reads that file; it
  never opens a book to find one. Books indexed by older firmware have no such file until they are indexed again.
- `lastRead` is specified as optional and this firmware still never emits it. It asked a vaguer question ("when was
  this book last read") than the sync path needs; `timestamp` answers the precise one and is what clients should use.

The document is streamed a book at a time, so it is never assembled in RAM. It is staged as
`/.crosspoint/ble-library.json` for the duration of the transfer and removed when the link stops (deep sleep). A
`start_get` at `offset: 0` rebuilds the listing; a non-zero `offset` resumes the document that the preceding
`offset: 0` request staged, so a resumed download never straddles two different snapshots.

## Position encoding

`location` is the reader's saved position with nothing done to it. On disk that position lives in
`progress.bin` inside the book's cache directory; its layout is private to the reader activity that wrote it
(EPUB: spine index, page, chapter page count and optionally a visible-text offset, in 4, 6 or 10 bytes; XTC and
`.txt`/`.md`: a 4-byte page index). The protocol does not know or care what the bytes mean.

To survive JSON they are **lowercase hex, two characters per byte, no separators and no prefix**. `location` is
therefore an even-length string of 8, 12 or 20 characters. Hex rather than base64 because a position is at most ten
bytes, hex stays readable in a log or a bug report, and it needs no padding or alphabet caveats.

```text
progress.bin bytes  03 00 17 00 23 00 00 00
location            "0300170023000000"
```

The round trip is byte-for-byte: whatever `library` reports in `location`, handing that same string back in a
`progress` upload writes exactly those bytes back into `progress.bin`. Clients must not construct, truncate, pad or
otherwise edit a `location` — only store one and return it.

The device does check the *length* against the book's type before writing (EPUB accepts 4, 6 or 10 bytes; XTC, `.txt`
and `.md` accept 4), because for EPUB the byte count selects the format and a wrong length would resume the book
somewhere arbitrary. A length the book's reader does not write is rejected as `invalid`.

## Device clock

A build without the network stack has no NTP, so **the phone sets the device clock**:

```json
{"op":"set_time","epoch":1725600000}
```

An optional `utc_offset_q` carries the phone's time zone in quarter-hours biased by 48 (`48` = UTC+0, `52` = UTC+1,
`28` = UTC-5). Values `0`–`96` are saved as the device's clock offset; anything else is ignored.

`epoch` is UTC seconds. It is accepted only in `[1577836800, 4102444800)` — 2020 through 2099 — and refused outside
that as `invalid epoch`; the lower bound is what distinguishes a real wall clock from an RTC that has never been set,
and the upper bound stops a garbled value becoming a timestamp no later save can beat. Like every other op it requires
`hello` first. Success changes no state: the acknowledgement is the new `device_time` in the `status` the client is
already subscribed to.

`status` always carries `clock_supported`, and carries `device_time` **only when the device actually knows the time**:

```json
{"clock_supported": true, "device_time": 1725600000}
```

- `clock_supported: false` — this board has no RTC. `set_time` is refused with `no clock on this device`, saved
  positions are never timestamped, and this device always loses a conflict. Hide the control.
- `clock_supported: true` with no `device_time` — an RTC that is present but unreadable, or one whose date registers
  hold nonsense. Rare (see seeding, below). Send `set_time`.
- `device_time` present — the normal case. Compare it against your own clock and re-send `set_time` if it has drifted.

`device_time` is never `0`. An unknown time is an absent field, because `0` would read as a real instant in 1970.

### The clock is running before you set it

The RTC ships with a stopped oscillator, so on a brand-new device it would report no time at all until a client sent
`set_time` — and every position the user read in the meantime would be saved unstamped, losing every later conflict.
That limitation is gone: **the firmware seeds the RTC from its own build epoch at boot.**

A device cannot be older than the firmware running on it, so the build epoch is a sound lower bound for wall time. At
every boot the firmware compares the two:

| RTC holds | What happens |
| --- | --- |
| No plausible time (stopped oscillator, garbled registers) | Seeded to the build epoch. |
| A time *before* the build epoch | Advanced to the build epoch — a clock predating its own firmware is wrong. |
| A time at or after the build epoch | Left alone. |

**The clock is never moved backwards**, and a `set_time` from a client always wins: it is the more accurate source and
it is what corrects the seed. The consequence for clients is that `device_time` may be *behind* real time — as far
behind as the firmware's age — until the app corrects it. It is never "unknown".

**Still send `set_time` early.** A seeded clock keeps the device's saves stamped and comparable, but the stamps are
only as good as the seed: two devices on different firmware builds order against each other by build date, not by when
the user actually read. Send `set_time` as soon as `status` arrives and before uploading a `progress` batch.

The clock is a hardware RTC with full date registers (PCF8563 on this board, DS3231 and RX8130 on others), so this is
a genuine wall clock, not a since-boot counter — the firmware does not fall back to one, and the build epoch is the
only date it ever supplies itself. The RTC is read at most every 10 seconds and the reading is carried forward with
the millisecond counter in between, so two saves inside one poll window still order correctly.

## `progress`

`{"op":"start_put","kind":"progress","size":…,"sha256":…}` uploads reading positions from the app to the device. It
uses the ordinary upload machinery — binary frames on `data-in`, credit-based flow control via `ack_bytes`, SHA-256
over the whole document, `commit` — and the same `hello` gate. `name` is not used: the batch is scratch, staged at a
fixed path, and deleted once applied. The document must be at most 512 KB and 8192 entries.

The payload is a JSON array, one object per book:

```json
[
  {"filename":"Dune.epub","location":"0300170023000000","timestamp":1725600000},
  {"filename":"Classics/Ulysses.epub","location":"1a000400","timestamp":1725600123}
]
```

| Field | Type | Meaning |
| --- | --- | --- |
| `filename` | string | Path relative to `/Books`, exactly as `library` reported it. Required. |
| `location` | string | Hex-encoded saved position (see [Position encoding](#position-encoding)). Required. |
| `timestamp` | number | UTC epoch seconds at which the app believes that position was reached. Required. |
| `pct` | number | Optional. Progress through the book, `0`–`1`, stored beside the position so the library screen can show it without opening the book. Absent leaves the stored value alone. |
| `spine`, `spine_n`, `spine_frac` | number | Optional, all or none. A position from another reading system: spine item index, the spine item count the sender measured, and a fraction `0`–`1` through that item. The reader checks `spine_n` against its own copy when it opens the book and ignores the jump if they differ. |

Nothing is written until the whole batch is on the card and its SHA-256 matches, so a transfer cut short cannot
half-apply.

### Conflict rule

An entry is applied **only if `timestamp` is strictly newer than the timestamp stored beside the device's own saved
position for that book.** Otherwise it is skipped and reported `skipped_older`.

- Both sides normally have a real timestamp: the device stamps its own saves from a clock that is running from first
  boot (see [Device clock](#device-clock)), so "two dates, compared" is the ordinary case.
- A device timestamp that is **unknown counts as the oldest possible**, so any valid incoming timestamp wins. Unknown
  now means only: the position was saved by firmware older than this feature, its sidecar was lost or torn, or the
  board has no RTC.
- **Equal timestamps do not apply.** Echoing back what `library` just reported is a no-op, not a rewrite.
- An entry whose `timestamp` is outside the range `set_time` accepts is `invalid`, not applied. There is no path that
  writes a position without a usable timestamp.

### Per-entry results

One bad or unknown entry costs that entry only, never the batch: a book the phone knows about but this card does not
is the ordinary case. Every entry gets one of:

| Result | Meaning |
| --- | --- |
| `applied` | Written, and stamped with the incoming `timestamp`. |
| `skipped_older` | The device's own save is at least as new. |
| `not_found` | No such file under `/Books`. |
| `unsupported` | A file extension the reader cannot open, so it has no position format. |
| `invalid` | Malformed `filename`, `location` or `timestamp`, or a `location` length the book's reader does not write. |
| `write_failed` | The position could not be persisted (SD error). |

The outcomes are **not** in `status`: a notification carries at most `ATT_MTU - 3` bytes, so a shelf-sized array would
be truncated on the wire with no error. `status` reports only the headline after `commit` —

```json
{"state":"saved","kind":"progress","entries":42,"applied":37}
```

(Deliberately terse: the byte counts an upload normally reports are dropped for this kind so the whole status still
fits in one notification.)

— and the full list is fetched as a download:

```json
{"op":"start_get","kind":"progress_result"}
```

whose payload is a JSON array parallel to the upload, in the same order:

```json
[
  {"filename":"Dune.epub","result":"applied"},
  {"filename":"Classics/Ulysses.epub","result":"skipped_older"}
]
```

It is staged at `/.crosspoint/ble-progress-result.json` and removed when the link stops (deep sleep), so fetch it
in the same session. A batch that fails as a whole (malformed JSON, over the entry cap, or an SD write error) reports
`state: "error"` and produces no result document.

### Currently-open book

**A `progress` batch is refused outright while a book is open.** `start_put` with `kind: "progress"` answers
`state: "error"` with `error: "book open"`; retry when the user has left the reader.

This used to be guaranteed by the architecture rather than checked: the Bluetooth Transfer screen was reached through
`replaceActivity()`, which tore the reader down — final `progress.bin` written — before BLE advertising ever started.
Now that the link outlives every screen, that guarantee is gone, and a batch applied underneath a live reader would be
silently overwritten by that reader's own position when it exits. Refusing is worse for the client than succeeding and
much better than appearing to succeed.

Refused rather than deferred, deliberately: a client already knows how to retry, and a queue of pending shelf writes is
a far larger thing to get right than a retry is.

### Where the timestamp lives

Beside `progress.bin`, in a nine-byte sidecar `progress.time` (magic `CPPT`, a version byte, then the UTC epoch
little-endian), not inside `progress.bin` itself. Two reasons:

1. All three readers dispatch on the **exact byte length** of `progress.bin` — the EPUB reader accepts 4, 6 or 10 and
   treats 10 as "position + visible-text offset". Appending four timestamp bytes would turn a 6-byte position into the
   10-byte form and resume the book at a text offset that is really a clock reading. Length is load-bearing there; it
   cannot carry a trailer.
2. Firmware that predates this must keep opening books written by firmware that has it. An ignored extra file is
   compatible in both directions; a longer `progress.bin` is not.

Backwards compatibility follows from that: **every position saved before this existed has no sidecar, and reads as
"unknown"** — which the conflict rule treats as older than any real timestamp, never as epoch 0. A sidecar that is
short, has the wrong magic, or holds an implausible epoch also reads as unknown. Going forward, though, unknown is the
exception rather than the rule: with the clock seeded at boot, every save this firmware makes on a board with an RTC
writes a sidecar.

`progress.bin` keeps its crash-safe temp-and-rename write (issue #2275). The sidecar is written in place afterwards:
it is nine bytes in a single sector, and a torn write there fails the magic check and degrades to "unknown" — a
skipped sync, not a lost book. A save made while the device has no working clock **deletes** any existing sidecar
rather than leaving a stale one, because a stale stamp would claim a position the user reached just now was reached
much earlier, and let an incoming sync overwrite genuinely fresher reading. That path is now reachable only on a board
with no working RTC.

## The Store: requests over the notify channel

The Store screen (home > Store) browses the phone app's Calibre library live. It is the one part of this
protocol where **the device asks and the app answers**, which needs a mechanism the rest of it does not.

### Why it is shaped this way

BLE GATT is client-driven. The phone is the central and the reader is the peripheral, so the reader cannot
call out — it can only reply to what the phone writes. Everything else in this document fits that: the app
decides to push a book, decides to pull the library, and the reader answers.

A store inverts it. The reader knows which six books are on screen, and it knows the instant the user turns
the page. The app is the only thing that can reach Calibre. Polling from the app would mean either a store
that lags a page turn by seconds or a radio that never sleeps.

So the `status` characteristic — already `notify`, already subscribed to by the app — is used as a **request
channel**. When the reader wants something it puts a `pending` object in the status document and notifies:

```json
{"state":"connected","protocol_version":2,"store_supported":true,
 "pending":{"req":7,"op":"catalog_page","offset":12,"limit":6,
            "thumb_w":72,"thumb_h":108,"desc_max":160,"timeout_ms":20000}}
```

The app sees the notification, fetches from Calibre, and answers with an **ordinary upload**: `start_put`
with the matching `kind`, framed writes on `data-in`, credit flow control through `ack_bytes`, SHA-256 over
the whole payload, `commit`. The one thing the store adds to an upload is a `req` field. The `hello` gate,
the framing, the hashing and the flow control are all reused untouched — the store is a new question, not a
new transport.

**A notification is a doorbell; a GATT read of `status` is authoritative.** A notification carries at most
`ATT_MTU - 3` bytes — 514 at the 517 the reader asks for, 182 on a client that negotiates the iOS default,
and 20 on one that never exchanges MTUs at all. The full status document does not fit a notification, so it is
**never** notified. The reader keeps the notified payload at or under **180 bytes**, which is `ATT_MTU - 3`
for the ~185-byte MTU that iOS and most Android stacks settle on, so the doorbell survives a small MTU, a
re-negotiation downwards, and a reconnect that never exchanges.

To stay under that cap the reader drops whole fields, in this order, and stops as soon as the document
fits. It never truncates: a client always receives parseable JSON.

| Dropped | Fields |
| --- | --- |
| first | `protocol_version`, `store_supported`, `clock_supported`, `device_time` |
| then | `has_trusted_host`, `trusted_host`, `reader_proof`, `paired`, `name`, `path`, `book` |
| then | `pending` keeps only `req`, `op` and the `id`/`offset` an answer must quote back |
| then | the transfer counters (`kind`, `received`, `sent`, `size`, `chunk_size`, `ack_bytes`, `resumable`, `entries`, `applied`, `position_applied`) and the `error` / `auth_error` text |

`chunk_size` is added to a `sending` status only when it fits the document that was chosen without dropping
anything more, so it never costs another field.
| never | `state`, the heartbeat fields `lib_n`, `lib_h`, `pct`, `open`, `sleeping`, and `pending` |

If even the floor does not fit, no notification is sent; the GATT read still carries the session.

`firmware_name`, `firmware_ota_supported`, `resume_supported`, `upload_kinds`, `download_kinds`, `device_id` and
`device_nonce` are **read-only**: they are in the document a GATT read returns and in no notification at any size.
None of them change within a session.

The GATT read is bounded too, at 512 bytes. To fit it drops, in order, `firmware_name` / `firmware_ota_supported` /
`resume_supported`, then `upload_kinds` / `download_kinds`, then `clock_supported` / `device_time`. It never drops
identity, `device_nonce` or `auth_error`.

These rules apply after authentication. Before it, `status` carries only the fields in
[Status before authentication](#status-before-authentication).

In practice, at 180 bytes, a `pending` request is notified with its geometry intact and a transfer is
notified with its byte counters intact — the two things a live session cannot work without, since
`received` is the credit ack an upload waits on. **Read the characteristic once after subscribing, keep
the session-constant fields, and merge each notification over them.** `scripts/ble_transfer.py` does
exactly this; see `SESSION_FACT_KEYS`.

### Correlation

`req` is a counter, starting at 1, incremented for every request the reader issues in a session. The reader
has **at most one request outstanding at a time**.

An answer naming any other `req` is refused with `{"state":"error","error":"stale request"}` and changes
nothing on screen — the pending request stays pending. That is what stops a slow reply, arriving after the
user has already paged on, from repainting the screen with the page they left. `req` is not persisted; a
reconnect starts a new session and a new counter.

### Retry

A GATT notification is unacknowledged: there is no ATT-level confirmation that the app received it. So an
outstanding request is **re-notified every 4 s, up to 4 times**, carrying the same `req`. An app that saw
the first copy answers once; the later copies name a request it has already answered and are ignored.

### Timeout

| Request | Deadline |
| --- | --- |
| `catalog_page`, `catalog_detail` | 20 s from issue to the answering `start_put` |
| `catalog_fetch` | 45 s from issue to the answering `start_put` |

The fetch window is longer because Calibre may convert a format before the app can begin sending. **The
clock stops when the upload starts** — from there the ordinary transfer machinery reports progress and owns
the failure.

A request that is not answered by its deadline fails **visibly**: the screen reads *The phone did not
answer* with the reason underneath and a Retry hint. It never hangs and it never silently shows stale
content. The request id still advances, so the reply that eventually arrives is refused as stale rather than
painted over whatever the user did next.

The app can also give up early rather than let the reader sit out the whole window:

```json
{"op":"catalog_error","req":7,"error":"calibre unreachable"}
```

`req` must be the outstanding request or the message is ignored. `error` is truncated to 96 bytes and shown
to the user verbatim.

### What the device shows when the app is absent or slow

**The Store requires the app to be open and connected.** There is no cached catalogue and no offline
browsing, by construction: nothing about the catalogue is written to the card except the covers of the page
currently on screen, and those are deleted when the screen closes or the link drops.

| Situation | Screen |
| --- | --- |
| No app connected, or connected but not yet through `hello` | *The Store needs your phone*, with one action that leads to pairing |
| Request outstanding | *Asking your phone* / *Fetching from Calibre*, Back cancels |
| Deadline passed, or `catalog_error` | *The phone did not answer* + reason, Select retries, Back leaves |
| Link dropped mid-browse | Everything on screen is discarded and it returns to *The Store needs your phone* |

## The catalogue container

A page is text (titles, authors, a blurb) and pictures (a cover per book). Base64 inside JSON would cost a
third more bytes on a link where bytes are the whole constraint, and would force the reader to hold a
decoded page in RAM. So a `catalog_page` / `catalog_detail` payload is one binary blob:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | magic, ASCII `CPCT` |
| 4 | 1 | version, currently `1` |
| 5 | 1 | flags, reserved, must be `0` |
| 6 | 2 | `jsonLen`, little-endian uint16 |
| 8 | `jsonLen` | the JSON header, UTF-8, no trailing NUL |
| 8 + `jsonLen` | … | the thumbnails, concatenated in `items` order |

Each thumbnail occupies exactly as many bytes as its item's `thumb` field says; an item with `"thumb":0`
(or no `thumb`) contributes nothing and is listed without art. The blob's total length must equal
`8 + jsonLen + sum(thumb)`.

Caps: `jsonLen` ≤ 6144, any single `thumb` ≤ 8192, the whole container ≤ 64 KB. A container that breaks any
of them is refused whole — nothing partial is ever shown.

### How the device handles it without holding it

The blob is staged on SD by the ordinary upload path (part file, hash, rename on commit), exactly like a
`progress` batch. Only then is it opened: the header is read into RAM (kilobytes, capped), and each
thumbnail is copied **card-to-card** into its own small `.bmp` under `/.crosspoint/store/`, 512 bytes at a
time. The staged blob is deleted immediately afterwards. The covers are then drawn straight off the card by
the same `Bitmap` + `GfxRenderer::drawBitmap1Bit` path the book covers use.

Nothing larger than the JSON header and one 512-byte copy buffer is ever resident, so a page costs the same
RAM whether its covers are 1 KB or 8 KB each. This is the same "stage, then serve" shape as
`src/util/BookLibraryIndex.cpp`.

### Thumbnail format

**1-bit uncompressed Windows BMP, bottom-up, 2-entry palette (index 0 black `0x000000`, index 1 white
`0xFFFFFF`), `biCompression = BI_RGB`.** The app must dither — it has the CPU and the original artwork, the
reader has neither.

| Use | Size | Row stride | Bytes |
| --- | --- | --- | --- |
| List thumbnail (`catalog_page`) | 72 × 108 | 9 | 62 + 9 × 108 = **1034** |
| Detail cover (`catalog_detail`) | 144 × 216 | 18 | 62 + 18 × 216 = **3950** |

The reader sends the dimensions it wants in `thumb_w` / `thumb_h` on every request, so these are the current
values rather than a contract. A BMP of some other size still draws — `drawBitmap` scales it down to fit —
but it costs bytes for pixels that are then thrown away.

Why 1-bit rather than greyscale: a 4-bit 72 × 108 cover is 3.9 KB against 1.03 KB, which is the difference
between a page arriving in about a second and one taking four. Why 72 px wide: it is a whole number of bytes
per row, and 72 × 108 is the largest 2:3 cover that lets six rows fit a 480 × 800 portrait screen under the
header and the button hints.

**Per-page payload: six covers at 1034 bytes plus roughly 3 KB of JSON, so about 9 KB.**

## `catalog_page`

Request (in `status`):

```json
{"pending":{"req":7,"op":"catalog_page","offset":12,"limit":6,
            "thumb_w":72,"thumb_h":108,"desc_max":160,"timeout_ms":20000}}
```

`offset` is a zero-based index into the app's whole (possibly filtered or sorted — that is the app's choice,
and it must be stable within a session) library view. `limit` is always 6 today.

Answer: `{"op":"start_put","kind":"catalog_page","req":7,"size":…,"sha256":…}` then the container, whose
JSON header is:

```json
{"req":7,"offset":12,"total":842,
 "items":[
   {"id":"1234","title":"Dune","author":"Frank Herbert",
    "description":"Set on the desert planet Arrakis…","filename":"Dune.epub",
    "format":"epub","size":1048576,"thumb":1034}
 ]}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `req` | number | Must equal the request's `req`. Anything else is `stale request`. |
| `offset` | number | Echo of the request's `offset`; the reader shows it in the page footer. |
| `total` | number | Books in the whole view. **This is what makes pagination possible** — without it the reader cannot know whether a next page exists. |
| `items[].id` | string | Opaque handle, ≤ 64 bytes, printable ASCII, no `"` or `\`. Sent straight back in `catalog_detail` and `catalog_fetch`. Required. |
| `items[].title` | string | Truncated to 160 bytes. Falls back to `id` when empty. |
| `items[].author` | string | Truncated to 128 bytes. May be empty. |
| `items[].description` | string | **Truncated by the app to `desc_max` = 160 bytes**, and re-truncated on the device on a code-point boundary if it is not. One or two lines under the title is all a row can show. |
| `items[].filename` | string | What the app would name the file if it sent this book. Same rules as the `book` upload kind (≤ 96 bytes, alphanumeric plus `. _ -` and space, no leading dot). An entry without a usable one is listed but cannot be fetched. |
| `items[].format` | string | `epub`, `txt`, … Display only. |
| `items[].size` | number | Bytes, display only. |
| `items[].thumb` | number | Length of this item's BMP in the blob. `0` or absent means no cover. |

`onDevice` is deliberately **not** a wire field: whether `/Books/<filename>` already exists is answered on
the device, per entry, because the app cannot know what is on the card.

A page with no items and a non-zero `total` is refused as `empty catalog page`; a genuinely empty library is
`total: 0` with an empty `items`.

## `catalog_detail`

Tapping a row asks for the book:

```json
{"pending":{"req":8,"op":"catalog_detail","id":"1234",
            "thumb_w":144,"thumb_h":216,"desc_max":1024,"timeout_ms":20000}}
```

Answered with `kind: "catalog_detail"`, the same container, one `item` instead of `items`:

```json
{"req":8,"id":"1234",
 "item":{"id":"1234","title":"Dune","author":"Frank Herbert","format":"epub",
         "size":1048576,"filename":"Dune.epub","thumb":3950,
         "description":"…up to 1024 bytes…"}}
```

**Why a second round trip rather than reusing what the page already sent.** The list carries a 160-byte
snippet; the detail view wants the whole blurb, and a cover four times the area. Carrying 1 KB descriptions
and 3950-byte covers for six books in every page payload would take a page from ~9 KB to ~30 KB — more than
triple the cost of the frequent action (turning a page) to save one round trip on the rarer one (opening a
book). On a link this slow the page turn is what has to be fast. The round trip costs roughly a second,
which is about what the panel spends on a full repaint anyway, and the screen says *Asking your phone* while
it happens.

## `catalog_fetch`

The detail view's button. The reader publishes:

```json
{"pending":{"req":9,"op":"catalog_fetch","id":"1234","name":"Dune.epub","timeout_ms":45000}}
```

and the app answers with the **existing `book` upload kind**, plus the `req`:

```json
{"op":"start_put","kind":"book","req":9,"name":"Dune.epub","size":1048576,"sha256":"…"}
```

In store mode a `book` upload is accepted **only** as the answer to an outstanding `catalog_fetch`, and only
when `name` matches the `name` the request published. Anything else is refused (`stale request` /
`unexpected book`): the user asked for one specific book and an unsolicited push must not land on the card in
its place. Outside store mode the `book` kind is unchanged and needs no `req`.

From `start_put` onwards this is an ordinary upload. The Store screen shows a progress bar fed by the same
byte counts `status` reports, and on `commit` the file is renamed into `/Books` and the screen becomes *Added
to your books* with Select to open it.

**A book that is already on the device** never reaches this path: the catalogue page marked it `onDevice`
from the card, the detail view's button reads *On this device* and opens the local copy instead of asking for
a copy. If the file appears between the page arriving and the button being pressed, the ordinary `exists`
error surfaces on the Store screen.

## What the Store leaves on the card

Only the covers of the page and the detail currently on screen, under `/.crosspoint/store/` and
`/.crosspoint/store/detail/`, plus the staged container while a transfer is in flight. All of it is deleted
when the Store screen closes, and the page/detail covers are also dropped the moment the link drops. Nothing
about the catalogue survives a session.

## Explicit Non-goals

This protocol deliberately stays within CrossPoint Reader's project scope. CrossPoint does not support packages,
plugins, or package-state diagnostics, so the BLE service does not add package uploads or package-state downloads. It
also does not expose arbitrary SD-card browsing, arbitrary path reads, or arbitrary path writes.

Those operations belong in Marginalia or other forks that choose a package/plugin model. They are not reserved for a
later CrossPoint BLE iteration.

## Status Capabilities

Status JSON includes capability fields so clients can hide unsupported controls:

```json
{
  "protocol_version": 2,
  "firmware_name": "CrossPoint Reader",
  "firmware_ota_supported": true,
  "resume_supported": true,
  "upload_kinds": ["book", "bmp", "firmware", "progress", "catalog_page", "catalog_detail", "settings", "book_meta"],
  "download_kinds": ["about", "book", "crash_report", "library", "progress_result", "settings"],
  "store_supported": true,
  "clock_supported": true,
  "device_time": 1725600000
}
```

`device_time` is present only when the device knows the time; see [Device clock](#device-clock).
`store_supported` says this firmware speaks the Store request protocol. There is no `"mode"` field: the link is not a
screen and has no mode. `firmware_ota_supported` means "this reader accepts the `firmware` upload kind"; what it does
with it is the file drop described above. The
firmware version is in the [`about`](#about) download, not in `status`.

Features that are not a kind, such as `book_position` or `pair_prompt`, are listed in the `features` array of the
[`about`](#about) download, not in `status`. After `hello` the `status` read usually exceeds 512 bytes and drops
`upload_kinds` / `download_kinds`, so do not treat their absence as "unsupported".

The states `confirming`, `updating`, `restarting`, `save_host_prompt` and `forget_host_prompt` do not exist; the
firmware prompt is shown by the reader on its own, not by the link.

**Every field above comes from a GATT read of `status`, not from a notification.** Read the characteristic after
subscribing and merge notifications over what it gave you; see
[The Store](#the-store-requests-over-the-notify-channel) for the exact rule.

## Heartbeat fields

After authentication these fields are in both the GATT read and every notification, and are never dropped to fit
(except `book`, which is dropped with the identity fields):

| Field | Type | Meaning |
| --- | --- | --- |
| `lib_n` | number | Number of books under `/Books`. `lib_n` and `lib_h` are absent while the library is empty or not yet fingerprinted. |
| `lib_h` | number | Fingerprint of `/Books` (file names, sizes and modification times). A change means the app's copy of the library is stale and a `library` download is worth doing; no change means it can be skipped. |
| `book` | string | File name (no folder) of the book last opened on the reader. Stays set after the book is closed. |
| `pct` | number | Progress through that book, `0`–`1`, from its saved position. Absent when unknown. |
| `open` | bool | `true` while a book is open on screen. Absent otherwise. |
| `sleeping` | bool | `true` in the notification sent just before the reader goes into deep sleep. The link then drops. Absent otherwise. |

When they are sent: every 60 seconds while a phone is connected, when a book opens or closes, when an EPUB position is
saved, and once on the way into sleep.

Clients should still handle `state: "error"` for rejected operations.
