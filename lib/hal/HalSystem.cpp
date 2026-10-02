#include "HalSystem.h"

#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_debug_helpers.h"
#include "esp_memory_utils.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"
#if !__riscv
#include <xtensa_context.h>  // XtExcFrame for the stack capture below
#endif

#define MAX_PANIC_STACK_DEPTH 32
#define PANIC_CAPTURE_MAGIC 0x50414E49u

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];
// Which task was running, and -- for a hardware exception, which never reaches
// panic_abort and so leaves panicMessage empty -- what the exception was. These
// three are the difference between "something crashed" and a named fault at a
// resolvable address in a named task.
RTC_NOINIT_ATTR char panicTaskName[16];
RTC_NOINIT_ATTR uint32_t panicFaultPc;
RTC_NOINIT_ATTR uint32_t panicFaultCause;
RTC_NOINIT_ATTR uint32_t panicFaultAddr;
// RTC_NOINIT is uninitialized on cold boot, so only this exact marker proves a
// panic diagnostic was captured before the reset.
RTC_NOINIT_ATTR volatile uint32_t panicCaptureMarker;

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";

// Panic context: no locks, no flash. xTaskGetCurrentTaskHandle() and
// pcTaskGetName() are lock-free TCB reads and ESP-IDF places FreeRTOS in IRAM, so
// both are callable here; the name is bounded-copied the same way panicMessage is.
static void IRAM_ATTR capturePanicTask() {
  panicTaskName[0] = '\0';
  const TaskHandle_t task = xTaskGetCurrentTaskHandle();
  if (task == nullptr) return;
  const char* name = pcTaskGetName(task);
  // The name lives inside the TCB, which is DRAM. Anything else means the handle
  // is not a TCB and must not be followed.
  if (name == nullptr || !esp_ptr_in_dram(name)) return;
  int i = 0;
  for (; i < (int)sizeof(panicTaskName) - 1 && name[i]; i++) {
    panicTaskName[i] = name[i];
  }
  panicTaskName[i] = '\0';
}

void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';
  capturePanicTask();
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_abort(message);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }

  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }

  // Stack window dump, mirroring components/esp_system/port/arch/*/panic_arch.c.
  // Hardware exceptions never reach __wrap_panic_abort, so on both
  // architectures this dump is the only diagnostic a crash leaves on-device.
  //
  // The faulting PC, the exception cause and the address that was touched come
  // straight out of the exception frame. Three words, taken before the stack
  // window below, which is the part that can decide the dump is not worth taking.
#if __riscv
  const RvExcFrame* const regs = (const RvExcFrame*)frame;
  panicFaultPc = (uint32_t)regs->mepc;
  panicFaultCause = (uint32_t)regs->mcause;
  panicFaultAddr = (uint32_t)regs->mtval;
  const uint32_t sp = (uint32_t)regs->sp;
#else
  const XtExcFrame* const regs = (const XtExcFrame*)frame;
  panicFaultPc = (uint32_t)regs->pc;
  panicFaultCause = (uint32_t)regs->exccause;
  panicFaultAddr = (uint32_t)regs->excvaddr;
  const uint32_t sp = (uint32_t)regs->a1;
#endif
  capturePanicTask();
  constexpr uint32_t captureBytes = 1024;
  if (!esp_stack_ptr_is_sane(sp) || sp > UINT32_MAX - captureBytes ||
      !esp_ptr_in_dram(reinterpret_cast<const void*>(sp + captureBytes - 1))) {
    // The registers above are still worth keeping even with no stack to walk.
    panicCaptureMarker = PANIC_CAPTURE_MAGIC;
    __real_panic_print_backtrace(frame, core);
    return;
  }
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < captureBytes; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_print_backtrace(frame, core);
}
}

namespace HalSystem {

void begin() {
  // On a panic reboot, preserve diagnostics until checkPanic() has tried to write them to the SD card.
  // Ordinary boots clear any stale retained diagnostics.
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  if (isRebootFromPanic()) {
    auto panicInfo = getPanicInfo(true);
    auto file = Storage.open("/crash_report.txt", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
      const size_t written = file.write(panicInfo.c_str(), panicInfo.size());
      file.close();
      if (written == panicInfo.size()) {
        // Keep the crash data for CrashActivity, but mark it consumed so a
        // later watchdog reset cannot be mistaken for this panic.
        panicCaptureMarker = 0;
        LOG_INF("SYS", "Dumped panic info to SD card");
      } else {
        LOG_ERR("SYS", "Failed to write complete crash report (%zu of %zu bytes)", written, panicInfo.size());
      }
    } else {
      LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
    }
  }
}

void clearPanic() {
  panicCaptureMarker = 0;
  panicMessage[0] = '\0';
  panicTaskName[0] = '\0';
  panicFaultPc = 0;
  panicFaultCause = 0;
  panicFaultAddr = 0;
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  clearLastLogs();
}

// The causes a reader actually hits. Anything else is reported as its number,
// which the ESP32 technical reference manual names.
static const char* faultCauseName(uint32_t cause) {
#if __riscv
  switch (cause) {
    case 1:
      return "InstructionAccessFault";
    case 2:
      return "IllegalInstruction";
    case 4:
      return "LoadAddressMisaligned";
    case 5:
      return "LoadAccessFault";
    case 6:
      return "StoreAddressMisaligned";
    case 7:
      return "StoreAccessFault";
    default:
      return "";
  }
#else
  switch (cause) {
    case 0:
      return "IllegalInstruction";
    case 2:
      return "InstrFetchError";
    case 3:
      return "LoadStoreError";
    case 6:
      return "IntegerDivideByZero";
    case 9:
      return "LoadStoreAlignment";
    case 20:
      return "InstrFetchProhibited";
    case 28:
      return "LoadProhibited";
    case 29:
      return "StoreProhibited";
    default:
      return "";
  }
#endif
}

static const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_PANIC:
      return "PANIC (exception/abort)";
    case ESP_RST_CPU_LOCKUP:
      return "CPU_LOCKUP";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT (other)";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_SW:
      return "SW";
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    default:
      return "OTHER";
  }
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return panicMessage;
  } else {
    std::string info;

    info += "CrossPoint version: " CROSSPOINT_VERSION;
    // A lockup or hardware watchdog resets without running any panic hook, so
    // the reason and stack come back empty; the reset cause is then the only
    // way to tell those apart from a true panic.
    info += "\n\nReset reason: " + std::string(resetReasonName(esp_reset_reason()));
    info += "\n\nPanic reason: " + std::string(panicMessage);
    info += "\nTask: " + std::string(panicTaskName[0] ? panicTaskName : "(not captured)");
    // Empty on a lockup or a hardware watchdog, which reset without running the
    // exception handler, and on an abort, whose reason is the line above.
    if (panicFaultPc != 0) {
      char fault[96];
      snprintf(fault, sizeof(fault), "\nFault: pc 0x%08X, cause %u %s, address 0x%08X",
               static_cast<unsigned>(panicFaultPc), static_cast<unsigned>(panicFaultCause),
               faultCauseName(panicFaultCause), static_cast<unsigned>(panicFaultAddr));
      info += fault;
    }
    info += "\n\nLast logs:\n" + getLastLogs();
    info += "\n\nStack memory:\n";

    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
    for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
      if (panicStack[i].sp == 0) {
        break;
      }
      info += "0x" + toHex(panicStack[i].sp) + ": ";
      for (size_t j = 0; j < 8; j++) {
        info += "0x" + toHex(panicStack[i].spp[j]) + " ";
      }
      info += "\n";
    }

    return info;
  }
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP) {
    return true;
  }

  const bool watchdogReset =
      resetReason == ESP_RST_INT_WDT || resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT;
  return watchdogReset && panicCaptureMarker == PANIC_CAPTURE_MAGIC;
}

}  // namespace HalSystem
