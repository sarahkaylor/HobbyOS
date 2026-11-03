/* Shared row shape for the ABI differential (see xcompat_abi_probe.h). */
#ifndef XCOMPAT_ABI_ROW_H
#define XCOMPAT_ABI_ROW_H
struct abi_row {
  const char *name;
  unsigned long value;
};
#endif
