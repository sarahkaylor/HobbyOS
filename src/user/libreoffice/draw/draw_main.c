/*
 * src/user/libreoffice/draw/draw_main.c
 *
 * Standalone launcher for LibreOffice Draw (sdraw).
 */

#include "soffice.h"

int main(int argc, char **argv) {
    return run_draw_main(argc, argv, false);
}
