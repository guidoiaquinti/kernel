// SPDX-License-Identifier: GPL-3.0-or-later
//
// Construction and bring-up of the vendored Circle USB stack on the KoraOS HAL
// bridge: the host controller for this board (CUSBHCIDevice is the DWC2 on the
// Pi 3, the xHCI behind PCIe on the Pi 4), device enumeration, the USB
// keyboard feeding the tty and the mass storage drives for the block layer.
// QEMU's raspi4b has no PCIe, so the caller gates enumeration with `enumerate`.

#include "circle_env.h"

#include <circle/devicenameservice.h>
#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/machineinfo.h>
#include <circle/spinlock.h>
#include <circle/timer.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>

#include "drivers/usb_storage.h"
#include "mm/mmu.h"
#include "tty.h"

extern "C" void tfp_printf(const char *fmt, ...);

namespace {

// Cooked keystrokes from the USB keyboard (IRQ context) feed the tty input
// queue that read(fd 0) waits on. Multi-byte strings are escape sequences for
// arrows, function keys and the like, which nothing consumes yet: drop them.
void key_pressed_handler(const char *pString) {
    if (pString[0] != '\0' && pString[1] == '\0') {
        tty_input_push(pString[0]);
    }
}

// Shift+PageUp / Shift+PageDown scroll the screen's history. Circle's keymaps
// give these no cooked string, so they are picked out of the raw HID boot
// reports; the handler runs in mixed mode, so cooked keys still flow above.
constexpr unsigned char kHidPageUp = 0x4B;
constexpr unsigned char kHidPageDown = 0x4E;
constexpr unsigned char kHidShiftMask = 0x02 | 0x20;  // left / right shift

void key_status_raw(unsigned char ucModifiers, const unsigned char RawKeys[6]) {
    static unsigned char previous[6];
    for (unsigned i = 0; i < 6; i++) {
        unsigned char key = RawKeys[i];
        bool held_before = false;
        for (unsigned j = 0; j < 6; j++) {
            held_before |= key == previous[j];
        }
        if (key == 0 || held_before || !(ucModifiers & kHidShiftMask)) {
            continue;
        }
        if (key == kHidPageUp) {
            tty_scrollback(1);
        } else if (key == kHidPageDown) {
            tty_scrollback(-1);
        }
    }
    for (unsigned i = 0; i < 6; i++) {
        previous[i] = RawKeys[i];
    }
}

}  // namespace

void circle_usb_init(int enumerate) {
    // Circle is built multi-core (filesystem calls reach USB storage from any
    // core): its spinlocks only lock once enabled.
    CSpinLock::Enable();

    // Constructed once, with static storage duration. Order matters: name
    // service and interrupt system first, then logger and timer, then the USB
    // host controller that depends on them. bPlugAndPlay = FALSE so Initialize()
    // enumerates whatever is attached at boot (a keyboard must be plugged in).
    static CDeviceNameService DeviceNameService;
    static CInterruptSystem InterruptSystem;
    InterruptSystem.Initialize();

    static CLogger Logger(LogDebug, 0);
    Logger.Initialize(0);

    static CTimer Timer(&InterruptSystem);
    Timer.Initialize();

    // Board model, RAM size and (Pi 4) the firmware's device tree, which the
    // xHCI driver needs for the PCIe DMA window. CMachineInfo clears the
    // firmware's device-tree pointer in page 0 after reading it.
    mmu_set_firmware_page_writable(1);
    static CMachineInfo MachineInfo;
    mmu_set_firmware_page_writable(0);
    tfp_printf("circle: %s rev %u, %u MB RAM\n", MachineInfo.GetMachineName(),
               MachineInfo.GetModelRevision(), MachineInfo.GetRAMSize());
#if RASPPI >= 4
    TMemoryWindow dma = MachineInfo.GetPCIeDMAMemory(PCIE_BUS_XHCI);
    tfp_printf("circle: device tree %s; PCIe DMA: bus 0x%lx -> cpu 0x%lx, size 0x%lx\n",
               MachineInfo.GetDTB() != 0 ? "found" : "NOT found (using defaults)",
               (unsigned long)dma.BusAddress, (unsigned long)dma.CPUAddress,
               (unsigned long)dma.Size);
    TMemoryWindow mmio = MachineInfo.GetPCIeMemory(PCIE_BUS_XHCI);
    tfp_printf("circle: PCIe MMIO: cpu 0x%lx -> bus 0x%lx, size 0x%lx\n",
               (unsigned long)mmio.CPUAddress, (unsigned long)mmio.BusAddress,
               (unsigned long)mmio.Size);
#endif

    static CUSBHCIDevice USBHCI(&InterruptSystem, &Timer, FALSE /* no plug&play */);

    if (!enumerate) {
        tfp_printf("circle: USB host controller (%s) constructed, enumeration skipped\n",
                   RASPPI >= 4 ? "xHCI" : "DWC2");
        return;
    }

#if RASPPI >= 4
    // PCIe link and reset, the VideoCore loading the VL805's firmware, the xHCI
    // reset, then enumeration of the root ports (Circle logs each failure).
    tfp_printf("circle: initializing USB host controller (xHCI via PCIe)...\n");
#else
    tfp_printf("circle: initializing USB host controller (DWC2)...\n");
#endif
    if (!USBHCI.Initialize()) {
        tfp_printf("circle: USB host controller init FAILED\n");
        return;
    }
    usb_storage_collect();

    CUSBKeyboardDevice *pKeyboard =
        (CUSBKeyboardDevice *)DeviceNameService.GetDevice("ukbd", 1, FALSE);
    if (pKeyboard == 0) {
        tfp_printf("circle: no USB keyboard found (is one plugged in?)\n");
        return;
    }

    pKeyboard->RegisterKeyPressedHandler(key_pressed_handler);
    pKeyboard->RegisterKeyStatusHandlerRaw(key_status_raw, TRUE /* mixed mode */);
    tfp_printf("circle: USB keyboard ready -- input goes to the screen terminal\n");
}
