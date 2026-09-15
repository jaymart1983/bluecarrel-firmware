#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The wire shape of a Calibre catalogue page or book detail, as the phone app
// uploads it, and the parser that turns one into something the Store screen can
// draw.
//
// WHY A CONTAINER RATHER THAN JSON. A page is text (titles, authors, a blurb)
// and pictures (one cover thumbnail per book). Base64 inside JSON would cost a
// third more bytes on a link where bytes are the whole constraint, and would
// force the device to hold a decoded page in RAM. So the payload is one binary
// blob -- a short JSON header followed by the thumbnails, back to back -- pushed
// through the ordinary `start_put` upload path with its framing, credit flow
// control and SHA-256 unchanged. The device stages it on SD like every other
// upload, then reads the header (bounded, kilobytes) and copies each thumbnail
// out to its own small file. Nothing larger than one 512-byte buffer is ever
// resident, and the covers are then drawn straight off the card by the ordinary
// Bitmap/drawBitmap1Bit path.
//
//   offset  bytes      field
//   0       4          magic "CPCT"
//   4       1          version, currently 1
//   5       1          flags, reserved, must be 0
//   6       2          jsonLen, little-endian uint16
//   8       jsonLen    the JSON header, UTF-8, no trailing NUL
//   8+n     ...        thumbnails, concatenated in `items` order, each exactly
//                      as many bytes as its item's `thumb` field says
namespace BleCatalog {

// --- Thumbnail format --------------------------------------------------------
// 1-bit uncompressed Windows BMP, bottom-up, with the standard 2-entry palette
// (index 0 black, index 1 white). Chosen because the panel is 1-bit-friendly and
// the device already blits exactly this: GfxRenderer::drawBitmap() routes a
// 1-bit Bitmap to drawBitmap1Bit(), which is a row copy with no dithering, no
// scaling table and no intermediate buffer.
//
// The app does the dithering. It has the CPU and the original artwork; the
// device has neither. Greyscale was rejected on size: a 4-bit 72x108 cover is
// 3.9 KB against 1.03 KB for 1-bit, which is the difference between a page that
// arrives in about a second and one that takes four.
//
// The list thumbnail. 72 px is a whole number of bytes per row (9), and 72x108
// is the largest 2:3 cover that lets six rows fit a 480x800 portrait screen
// under the header and the button hints.
constexpr int THUMB_WIDTH = 72;
constexpr int THUMB_HEIGHT = 108;
// 9 bytes per row, 108 rows, plus the 62-byte header (14 file + 40 info + two
// palette entries). Exactly 1034 bytes, so a full page of covers is 6.1 KB.
constexpr size_t THUMB_EXPECTED_BYTES = 62 + 9 * 108;

// The detail view has the whole screen, so it gets a cover four times the area:
// 18 bytes per row, 216 rows, 3950 bytes.
// The shelf row's cover, drawn 1:1 and NEVER scaled. A 1-bit dither encodes
// tone in the placement of pixels, so resampling one is not a resize -- it is
// destruction. A 144x216 cover squeezed into a 64px row came out as a black
// block with a few stray dots, which is what a downscaled dither looks like.
// The phone renders at exactly this size instead.
constexpr int ROW_COVER_WIDTH = 88;
constexpr int ROW_COVER_HEIGHT = 132;
constexpr int COVER_WIDTH = 144;
constexpr int COVER_HEIGHT = 216;
constexpr size_t COVER_EXPECTED_BYTES = 62 + 18 * 216;

// --- Caps --------------------------------------------------------------------
// Six books per page: exactly what the list draws. A page is then about 6.1 KB
// of covers plus 3 KB of JSON -- roughly a second on this link. Asking for more
// than the screen shows would only make every page turn slower.
constexpr size_t PAGE_LIMIT = 6;

// A list row shows one line of blurb under the title, so the app truncates to
// this before sending. The full text is a `catalog_detail` away.
constexpr size_t MAX_LIST_DESCRIPTION_BYTES = 160;
// The detail view's blurb. A kilobyte is roughly 15 lines at this font, which is
// already more than fits without scrolling.
constexpr size_t MAX_DETAIL_DESCRIPTION_BYTES = 1024;

constexpr size_t MAX_JSON_BYTES = 6144;
// Generous next to COVER_EXPECTED_BYTES: it exists to stop a malformed header
// claiming a thumbnail the size of the card, not to police the exact format.
constexpr size_t MAX_THUMB_BYTES = 8192;
constexpr size_t MAX_CONTAINER_BYTES = 64UL * 1024UL;

constexpr size_t MAX_ID_BYTES = 64;
// Clamped on arrival like every other string: the app is not trusted to have
// applied its own limits.
constexpr size_t MAX_META_BYTES = 64;
constexpr size_t MAX_TAGS_BYTES = 96;
constexpr size_t MAX_TITLE_BYTES = 160;
constexpr size_t MAX_AUTHOR_BYTES = 128;
constexpr size_t MAX_FILENAME_BYTES = 96;

struct Entry {
  std::string id;
  std::string title;
  std::string author;
  std::string description;
  // What the app would name the file if it sent this book. Validated against the
  // same rules the `book` upload kind applies, so a page cannot smuggle a path.
  // Empty when the app did not offer one, which also means "cannot be fetched".
  std::string filename;
  std::string format;
  uint32_t size = 0;
  // /Books already holds a file by that name. Answered on the device, from the
  // card, because the app cannot know what is on it.
  bool onDevice = false;
  // Where the extracted cover landed, or empty when this entry had none.
  std::string thumbPath;
  // --- detail-only metadata --------------------------------------------------
  // Sent only for a catalog_detail; a page row shows none of it and pays nothing
  // for it. Every field is optional -- an app that does not send one leaves it
  // empty, which is also what an older app produces, so the reader renders what
  // it is given rather than requiring any of it.
  std::string series;     ///< "Foundation #2", already formatted by the app
  std::string publisher;
  std::string published;  ///< year
  std::string language;
  std::string tags;       ///< comma-joined
  // book_meta only: Calibre's book UUID, as sent (at most 65 bytes kept, so an
  // over-long value still fails BleLink's check). Empty when absent.
  std::string calibreUuid;
};

struct Page {
  uint32_t req = 0;
  uint32_t offset = 0;
  // How many books the app's whole (possibly filtered) Calibre view holds. This
  // is what lets the device page without ever seeing the library.
  uint32_t total = 0;
  std::vector<Entry> entries;
};

// Parses a staged container.
//
// `path`      the committed upload
// `thumbDir`  where extracted covers are written (created if missing, and
//             emptied of previous covers first)
// `expectedReq` the request id the device is waiting on; a container naming a
//             different one is refused rather than shown
// `detail`    true for a `catalog_detail` payload (a single `item` rather than
//             an `items` array, and the larger description cap)
// `booksRoot` used to answer `onDevice` per entry
//
// Returns false with a short reason in `error` on any malformed input. Nothing
// larger than MAX_JSON_BYTES plus a 512-byte copy buffer is ever resident.
bool parseContainer(const char* path, const char* thumbDir, uint32_t expectedReq, bool detail, const char* booksRoot,
                    Page& page, std::string& error);

// Removes previously extracted covers. Called when the Store screen closes and
// before each new page is unpacked.
void clearThumbnails(const char* thumbDir);

}  // namespace BleCatalog
