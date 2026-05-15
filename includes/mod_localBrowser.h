/*
	Copyright (c) 2025 Natalia Pujol Cremades
	info@abitwitches.com

	See LICENSE file.
*/
#pragma once

#include <stdint.h>

// showLocalBrowser() exit codes — returned to the caller so it can decide
// what panel to activate next.
#define LB_EXIT_CLOSE	0	// ESC at root (or alloc fail) — stay on local
#define LB_EXIT_TAB		1	// TAB pressed — advance to next panel
#define LB_EXIT_ROM		2	// R pressed — jump to ROM panel
#define LB_EXIT_DSK		3	// D pressed — jump to DSK panel
#define LB_EXIT_CAS		4	// C pressed — jump to CAS panel
#define LB_EXIT_QUIT	5	// User confirmed exit — quit fhMOD entirely

uint8_t showLocalBrowser(void);

// Centred "Exit fhMOD? Y / N" confirmation popup. Blocks until the
// user picks Y (or ENTER) -> true, N (or ESC) -> false. The caller is
// responsible for repainting whatever was under the popup afterwards.
bool lb_confirmExit(void);

// Called once at fhMOD startup. Reads A:\UTILS\FHCLEAN.LST (if present),
// deletes every absolute path listed in it, and wipes the manifest.
// Cleans up LOADCAX / FHCAS.BAS / FHRUN.BAT residue dropped into game
// directories by previous fhMOD sessions.
void lb_cleanupResiduals(void);
