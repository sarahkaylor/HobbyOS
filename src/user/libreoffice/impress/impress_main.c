/*
 * src/user/libreoffice/impress/impress_main.c
 *
 * Standalone launcher for LibreOffice Impress (simpress).
 */

#include "soffice.h"

int main(int argc, char **argv) {
    return run_impress_main(argc, argv, false);
}
