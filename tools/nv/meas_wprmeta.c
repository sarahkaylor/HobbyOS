/* Measure the vendor 580.178.04 GspFwWprMeta layout by compiling its own header.
 * No RM sources needed: gsp_fw_wpr_meta.h has no #include and only uses the Nv* types. */
#include <stdio.h>
#include <stddef.h>
typedef unsigned char      NvU8;
typedef unsigned short     NvU16;
typedef unsigned int       NvU32;
typedef unsigned long long NvU64;
#include "gsp_fw_wpr_meta_580.h"
int main(void) {
  printf("sizeof(GspFwWprMeta) = %zu\n", sizeof(GspFwWprMeta));
  printf("off magic                    0x%03zx\n", offsetof(GspFwWprMeta, magic));
  printf("off revision                 0x%03zx\n", offsetof(GspFwWprMeta, revision));
  printf("off nonWprHeapOffset         0x%03zx\n", offsetof(GspFwWprMeta, nonWprHeapOffset));
  printf("off nonWprHeapSize           0x%03zx\n", offsetof(GspFwWprMeta, nonWprHeapSize));
  printf("off gspFwHeapSize            0x%03zx\n", offsetof(GspFwWprMeta, gspFwHeapSize));
  printf("off frtsSize                 0x%03zx\n", offsetof(GspFwWprMeta, frtsSize));
  printf("off vgaWorkspaceSize         0x%03zx\n", offsetof(GspFwWprMeta, vgaWorkspaceSize));
  printf("off gspFwHeapVfPartitionCount 0x%03zx\n", offsetof(GspFwWprMeta, gspFwHeapVfPartitionCount));
  printf("off flags                    0x%03zx\n", offsetof(GspFwWprMeta, flags));
  printf("off pmuReservedSize          0x%03zx\n", offsetof(GspFwWprMeta, pmuReservedSize));
  printf("off verified                 0x%03zx\n", offsetof(GspFwWprMeta, verified));
  return 0;
}
