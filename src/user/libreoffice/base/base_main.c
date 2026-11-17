/*
 * src/user/libreoffice/base/base_main.c
 *
 * Standalone launcher for LibreOffice Base (sbase).
 */

#include "soffice.h"

int main(int argc, char **argv) {
    return run_base_main(argc, argv, false);
}
