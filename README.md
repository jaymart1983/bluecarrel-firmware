# Bluecarrel firmware

E-reader firmware for the **Xteink X4 Pro**, built to pair with the
[Bluecarrel](https://github.com/jaymart1983/bluecarrel-app) Android app. The phone keeps the reader's library,
reading positions and firmware in sync with your own Calibre-Web Automated server, over Bluetooth.

It started as a fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) and keeps its EPUB
engine and reading experience; see [Credits](#credits).

## Download

Prebuilt images are on the [Releases page](https://github.com/jaymart1983/bluecarrel-firmware/releases). Tags ending in
`-test` are pre-releases. Each release has:

| File | Use |
|---|---|
| `bluecarrel-x4pro-<version>-full.bin` | First install over USB (bootloader, partition table and firmware) |
| `bluecarrel-x4pro-<version>.bin` | Firmware only: USB reinstall, or hosting your own update page |
| `bluecarrel-x4pro-<version>.bin.sig` | Signature for the firmware image; copy to `/firmware/firmware.bin.sig` for a hand-copied update |
| `firmware.json` | Update-page manifest for the app, including the signature |
| `SHA256SUMS` | Checksums |

## Features

- **Bluetooth link to the app.** Pair once: open the reader's Settings page (Control Centre > Settings), start pairing
  in the app, and type the passkey the reader shows. The phone reconnects on its own and shows up by its own name.
- **Library from your server.** Books you keep offline in the app are copied to the reader; removing one removes it
  from both. The reader's Store browses your Calibre library through the phone.
- **Reading positions** sync to KOReader-compatible kosync through the app while you read.
- **Firmware updates over Bluetooth.** The app sends a new signed build; the reader verifies it and offers
  **Update Now / Later / Cancel**. Later installs the next time the reader sleeps, or turn on automatic install.
- **Touch page turns.** The screen is a 4x4 grid: the middle two cells on the left turn back, on the right turn
  forward. Links in books open with a tap. The touchscreen switch in the Control Centre turns touch off.
- **Controls.** Tap Power to open or close the Control Centre, hold it to sleep, double tap for the frontlight. There
  is no on-screen Back button; hold a side key for Back or Select outside a book.
- **No refresh flashes.** Nothing flashes the whole screen while you read. Use the Control Centre's Refresh tile, or
  hold its Home tile, when you want a clean screen.
- **USB Drive.** Plug into a computer while the reader is awake to mount the SD card.

The X4 Pro build is Bluetooth only: no Wi-Fi, web server, WebDAV, OPDS browser or HTTP updates are compiled in.

See the [User Guide](./USER_GUIDE.md) for everything else.

## Security

- **Encrypted pairing.** Bluetooth uses LE Secure Connections with a passkey shown on the reader, and pairing is only
  possible while the reader's Settings page is open. After that, the app and the reader prove a shared secret to each
  other on every connection.
- **Signed updates.** Firmware sent by the app or copied to `/firmware` installs only if it is signed with the project
  key and newer than the running version.
- **Not protected:** anyone with USB access can flash any firmware with `esptool` (there is no secure boot), and the
  **SD Card Firmware Update** picker in Settings installs unsigned images you choose on the device.
- The app only talks to the library server and update page over HTTPS.

Design: [docs/security-v2.md](./docs/security-v2.md).

## First install over USB

Back up the stock firmware first so you can return to it:

```bash
pip install esptool
esptool.py --chip esp32s3 --port <port> --baud 921600 read_flash 0 0x1000000 x4pro-stock-full-16MB.bin
```

Keep that file private: it is a copy of your device's flash.

Then flash the full image from a release:

```bash
esptool.py --chip esp32s3 --port <port> --baud 921600 write_flash 0x0 bluecarrel-x4pro-<version>-full.bin
```

`<port>` is `/dev/ttyACM0` on Linux or `/dev/cu.usbmodem*` on macOS. The reader only appears on USB while it is awake.

To reinstall firmware only (for example your own build), clear the OTA boot selection as well, or the reader may keep
booting the other app slot after a Bluetooth update:

```bash
esptool.py --chip esp32s3 --port <port> --baud 921600 erase_region 0xe000 0x2000
esptool.py --chip esp32s3 --port <port> --baud 921600 write_flash 0x10000 bluecarrel-x4pro-<version>.bin
```

To return to stock, write your backup back with `write_flash 0x0 x4pro-stock-full-16MB.bin`.

## Updates

After the first install, updates arrive over Bluetooth from the app. The app reads a `firmware.json` from an update
page you set in its Settings: point it at an `https://` folder holding a release's `firmware.json` and `.bin`. The
manifest must carry a signature, and the reader installs only images signed with the key it was built with.

## Build

Needs [PlatformIO](https://platformio.org/). Clone with submodules:

```bash
git clone --recursive https://github.com/jaymart1983/bluecarrel-firmware.git
cd bluecarrel-firmware
pio run -e x4pro
```

- The image is `.pio/build/x4pro/firmware.bin` (`x4pro-gh_release` is the release build, without serial logging).
- The version the reader shows and reports is a build stamp (`yyyyMMdd.HHmm`, UTC), generated into
  `src/network/BuildStamp.h` at build time. Set `X4_BUILD_STAMP` to pin it.
- Fixes this project needs in the `freeink-sdk` submodule are kept as patches in `scripts/freeink_patches/` and applied
  automatically.
- Releases are built by GitHub Actions when a tag `x4pro-<yyyyMMdd.HHmm>` is pushed (`.github/workflows/release.yml`),
  and signed with the `FIRMWARE_SIGNING_KEY` repository secret. A tag `x4pro-<stamp>-test` publishes a pre-release.

### Signing your own firmware

A reader only installs Bluetooth and `/firmware` updates signed with the key compiled into it. To run and update your own
builds:

1. Create an EC P-256 key and keep it private. Never commit it.

   ```bash
   openssl ecparam -name prime256v1 -genkey -noout -out firmware-signing-key.pem
   ```

2. Replace the public key in `src/network/FirmwareSigningKey.h` with yours (DER SubjectPublicKeyInfo bytes):

   ```bash
   openssl pkey -in firmware-signing-key.pem -pubout -outform DER | xxd -i
   ```

3. Build, install that build once over USB (see [First install over USB](#first-install-over-usb)), then sign each
   update:

   ```bash
   FIRMWARE_SIGNING_KEY_FILE=firmware-signing-key.pem scripts/make_firmware_json.sh <update-page-dir>
   ```

   This writes the `.bin`, its `.bin.sig` and `firmware.json` into the folder, and refuses if the key does not match
   `FirmwareSigningKey.h`.

A reader with your key refuses official releases, and the reverse.

Developer notes: [AGENTS.md](./AGENTS.md). Bluetooth protocol: [docs/ble-transfer-protocol.md](./docs/ble-transfer-protocol.md).

## Credits

Built on [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) by Dave Allie and the CrossPoint
contributors, and the [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk). The upstream README, which still applies
to the other boards this tree builds, is kept at
[docs/upstream-crosspoint-readme.md](./docs/upstream-crosspoint-readme.md).

## License

MIT. See [LICENSE](./LICENSE).
