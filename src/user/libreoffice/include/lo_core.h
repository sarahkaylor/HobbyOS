#ifndef LO_CORE_H
#define LO_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Common Result and Status Codes */
#define LO_OK                 0
#define LO_ERR_GENERIC       -1
#define LO_ERR_NULL_ARG      -2
#define LO_ERR_IO            -3
#define LO_ERR_CORRUPT       -4
#define LO_ERR_OUT_OF_BOUNDS -5
#define LO_ERR_SYNTAX        -6
#define LO_ERR_NOT_FOUND     -7
#define LO_ERR_OUT_OF_MEMORY -8

/* Document MIME Types per ISO/IEC 26300 (ODF) */
#define ODF_MIME_TEXT         "application/vnd.oasis.opendocument.text"
#define ODF_MIME_SPREADSHEET  "application/vnd.oasis.opendocument.spreadsheet"
#define ODF_MIME_PRESENTATION "application/vnd.oasis.opendocument.presentation"
#define ODF_MIME_GRAPHICS     "application/vnd.oasis.opendocument.graphics"
#define ODF_MIME_FORMULA      "application/vnd.oasis.opendocument.formula"
#define ODF_MIME_DATABASE     "application/vnd.oasis.opendocument.base"

#ifdef __cplusplus
}
#endif

#endif /* LO_CORE_H */
