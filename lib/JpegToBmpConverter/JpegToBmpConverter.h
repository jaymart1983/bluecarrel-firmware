#pragma once

#include <HalStorage.h>

class Print;
class ZipFile;

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true);

 public:
  static bool jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop = true);
  /// Same target size as jpegFileToBmpStream, but Atkinson-dithered to ONE bit.
  ///
  /// Two levels with dithering rather than four hard ones. On a dark, finely
  /// detailed cover the four-level quantiser bands and muddies, because the art
  /// has more tones than the budget; dithering spends spatial resolution -- of
  /// which a full-screen cover has plenty -- to buy tonal resolution instead.
  /// Dithered from the ORIGINAL image at final size, so nothing is quantised
  /// twice and nothing is resampled afterwards.
  static bool jpegFileTo1BitBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop = true);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                              int targetMaxHeight);
};
