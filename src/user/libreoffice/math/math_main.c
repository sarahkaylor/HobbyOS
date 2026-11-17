/*
 * src/user/libreoffice/math/math_main.c
 *
 * Standalone launcher for LibreOffice Math (smath).
 */

#include "soffice.h"

int main(int argc, char **argv) {
    return run_math_main(argc, argv, false);
}
