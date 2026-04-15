#pragma once
#include <stdbool.h>

// Show the server-selection overlay (F4).
// Returns true  when the user picked a *different* server
//         (caller must call selectPanel() to reload the list).
// Returns false when ESC was pressed or the same server was confirmed.
bool showServerBrowser(void);
