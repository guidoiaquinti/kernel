// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// C entry point into the vendored Circle USB stack. Constructs the KoraOS
// HAL-bridge singletons (CInterruptSystem/CTimer/CLogger, backed by KoraOS's
// interrupt controller, generic timer, and console), Circle's CMachineInfo, and
// the board's USB host controller (DWC2 on the Pi 3, xHCI on the Pi 4).

#ifdef __cplusplus
extern "C" {
#endif

// Construct the Circle USB stack on the KoraOS HAL bridge. When `enumerate` is
// nonzero, also initialize the host controller and enumerate devices, collect
// the mass storage drives (drivers/usb_storage.h) and feed an attached USB
// keyboard into the tty. QEMU's raspi3b models the Pi 3's DWC2 controller, but
// its raspi4b has no PCIe for the Pi 4's xHCI: callers pass 0 there.
void circle_usb_init(int enumerate);

#ifdef __cplusplus
}
#endif
