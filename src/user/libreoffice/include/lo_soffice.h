#ifndef LO_SOFFICE_H
#define LO_SOFFICE_H

#include <stdbool.h>
#include "soffice.h"
#include "lo_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

int run_soffice_main(int argc, char **argv);
void lo_soffice_init_ui(lo_ui_state_t *ui);

#ifdef __cplusplus
}
#endif

#endif /* LO_SOFFICE_H */
