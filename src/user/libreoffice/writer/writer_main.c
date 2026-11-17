/*
 * src/user/libreoffice/writer/writer_main.c
 *
 * Standalone launcher for LibreOffice Writer (swriter).
 */

#include "soffice.h"

int main(int argc, char **argv) {
    return run_writer_main(argc, argv, false);
}
