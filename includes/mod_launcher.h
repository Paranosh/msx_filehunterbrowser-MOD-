/*
    Copyright (c) 2025 Natalia Pujol Cremades
    info@abitwitches.com

    See LICENSE file.
*/
#pragma once

// Launch SofaRun (SR.COM /S) from the current directory.
// Overwrites fhMOD in memory — never returns on success.
// Expected usage:
//     restoreScreen();
//     launchSofaRun();
//     dos2_exit(1);   // only reached on load error
void launchSofaRun(void);
