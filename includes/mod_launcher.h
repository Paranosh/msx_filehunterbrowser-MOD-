/*
    Copyright (c) 2025 Natalia Pujol Cremades
    info@abitwitches.com

    See LICENSE file.
*/
#pragma once

// Launch SofaRun (SR.COM /S) from the current directory.
// Overwrites fhMOD in memory — never returns on success.
// Expected usage:
//     paintSplash();   // (or restoreScreen() if no splash desired)
//     launchSofaRun();
//     dos2_exit(1);    // only reached on load error
void launchSofaRun(void);

// Restore the screen to the user's original mode AND paint a centred
// "fhMOD - Loading..." splash (white on dark blue) with the cursor
// hidden. Use this instead of restoreScreen() right before exiting
// fhMOD so SR.COM / SROM / SRI loads behind a clean splash rather
// than the COMMAND.COM prompt.
void paintSplash(void);
