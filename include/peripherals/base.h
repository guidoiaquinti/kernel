#pragma once

#if defined(KORAOS_VIRT)
	// BCM peripherals are unavailable on virt; board drivers must be excluded.
	#define PBASE 0
#elif RPI_VERSION == 3
	#define PBASE 0x3F000000
#elif RPI_VERSION == 4
	#define PBASE 0xFE000000  // also QEMU's raspi4b machine
#else
	#define PBASE 0
	#error RPI_VERSION not defined
#endif
