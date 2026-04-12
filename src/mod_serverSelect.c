/*
    mod_serverSelect.c
    Lee la primera línea de REPOS.TXT (mismo directorio que FH.COM)
    y la usa como URL base. Si el archivo no existe, usa File-Hunter.
*/

#include <string.h>
#include "dos.h"
#include "fh.h"

static char urlBuf[128];

void selectServer(void)
{
    FILEH fh;
    char *p;

    if (!dos2_fileexists("REPOS.TXT")) return;

    fh = dos2_fopen("REPOS.TXT", 0x01);   /* 0x01 = no write (read-only) */
    if (fh > 20) return;                   /* error: file handles son 1-7 */

    if (dos2_fgets(urlBuf, sizeof(urlBuf), fh)) {
        /* Eliminar \r y \n del final */
        p = urlBuf;
        while (*p && *p != '\r' && *p != '\n') p++;
        *p = '\0';

        if (urlBuf[0]) {
            BASEURL = (const char *)urlBuf;
        }
    }

    dos2_fclose(fh);
}
