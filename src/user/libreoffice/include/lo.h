#ifndef LO_H
#define LO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "lo_core.h"
#include "lo_zip.h"
#include "lo_xml.h"
#include "lo_vcl.h"
#include "lo_writer.h"
#include "lo_calc.h"
#include "lo_impress.h"
#include "lo_draw.h"
#include "lo_math.h"
#include "lo_base.h"
#include "soffice.h"

#ifdef __cplusplus
extern "C" {
#endif

int lo_test_run_all(void);

#ifdef __cplusplus
}
#endif

#endif /* LO_H */
