#pragma once

#include <rg_system.h>

// The screen the host draws into, so the retro-go side can hand it to rg_emu_screenshot
// without a second copy of the frame buffer. NULL before oneTimeSetup.
rg_surface_t *retrogo_host_screen(void);
