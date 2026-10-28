#pragma once

#include <cstdint>

// Light sleep in place of the main loop's idle wait.
//
// Between page turns the reader has nothing to do, but the CPU keeps running at
// 80 MHz and the PLL stays up -- measured as the largest single draw while
// reading, well ahead of the frontlight. The loop already parks itself for
// IDLE_POWER_SAVING_MS of inactivity and then waits 50 ms at a time; this swaps
// that wait for a real doze of the same length. Same wake latency, same timing
// tolerances the firmware already lives with, but the core is off for nearly
// all of it.
//
// Buttons and the touch controller's interrupt wake it early, so a page turn
// does not pay the slice. Costs no heap and no task.
namespace cpu_doze {

// Everything that has to be true before the core may stop. Gathered by the
// caller because each answer lives in a different layer.
struct Preconditions {
  // The BLE radio is parked (BleDutyCycle): the controller has no scheduled
  // event to miss, which is the only state in which stopping its clock is safe.
  bool radioParked = false;
  // Plugged in, or a serial console attached. The power argument for dozing is
  // gone either way, and the USB CDC link does not survive the core stopping.
  bool usbPresent = false;
  // No button down, no finger on the glass.
  bool inputIdle = false;
  // The render task is not mid-frame. Dozing through an SPI transaction to the
  // panel would stop its clock half way.
  bool renderIdle = false;
};

// Arm the wake sources. Call once, after the input layer is up.
void begin();

// Doze for up to `ms`. Returns false without sleeping when a precondition
// fails or the platform refused, and the caller then waits the ordinary way.
bool doze(uint32_t ms, const Preconditions& pre);

struct Counters {
  uint32_t dozedMs;  // time the core was actually stopped
  uint32_t dozes;    // how many times
};
Counters counters();

}  // namespace cpu_doze
