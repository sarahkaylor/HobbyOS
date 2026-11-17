/*
 * src/user/libreoffice/calc/calc_main.c
 *
 * Standalone launcher for LibreOffice Calc (scalc).
 */

#include "soffice.h"

int main(int argc, char **argv) {
    return run_calc_main(argc, argv, false);
}
