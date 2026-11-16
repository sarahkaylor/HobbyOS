#ifndef SOFFICE_H
#define SOFFICE_H

#include <stdbool.h>
#include "lo_core.h"
#include "lo_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

void lo_soffice_init_ui(lo_ui_state_t *ui);
int run_soffice_main(int argc, char **argv);
int run_writer_main(int argc, char **argv, bool headless);
int run_calc_main(int argc, char **argv, bool headless);
int run_impress_main(int argc, char **argv, bool headless);
int run_draw_main(int argc, char **argv, bool headless);
int run_math_main(int argc, char **argv, bool headless);
int run_base_main(int argc, char **argv, bool headless);

#ifdef __cplusplus
}
#endif

#endif /* SOFFICE_H */
