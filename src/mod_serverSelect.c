/*
    mod_serverSelect.c  –  STUB DE DIAGNÓSTICO
    Versión mínima: solo inicializa BASEURL a File-Hunter.
    Si FH.COM funciona con esto, el problema estaba en el código UI anterior.
*/

#include <string.h>
#include "fh.h"

void selectServer(void)
{
    /* Stub: mantener File-Hunter por defecto, sin UI todavía */
    strcpy(BASEURL,
        "http://api.file-hunter.com/index4.php"
        "?base=1BA0&type=%s&msx=%s&char=%s&download=");
}
