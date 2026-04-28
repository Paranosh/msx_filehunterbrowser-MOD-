/*
    Copyright (c) 2025 Natalia Pujol Cremades
    info@abitwitches.com

    See LICENSE file.
*/
#pragma once

// Launch OCMINFO.COM (resolved via DOS PATH).
// Overwrites fhMOD in memory — never returns on success.
// Expected usage:
//     restoreScreen();
//     launchOcmInfo();
//     dos2_exit(1);   // only reached on load error
void launchOcmInfo(void);
