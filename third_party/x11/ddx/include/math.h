/* Server-stage overlay for <math.h>: the XSI constants dix/ptrveloc.c uses. */
#ifndef HB_XSERVER_MATH_OVERLAY_H
#define HB_XSERVER_MATH_OVERLAY_H
#include_next <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif
#endif
