#ifndef ACPI_H
#define ACPI_H

/*
 * Shared ACPI table locator (both arches).
 *
 * One place finds the RSDP and walks RSDT/XSDT so that the VT-d driver, the
 * PCIe bus layer and anything else that needs a firmware table do not each
 * carry a private copy of the same 150 lines.  The private copies had already
 * drifted: both read a table's header through a packed struct (a wide load at
 * a firmware-chosen address) and both ignored mmu_map_mmio_range's return
 * value, which is a different pointer from phys_to_virt() for a table above
 * 4 GiB.
 *
 * Parsing rules (HobbyOS kernel constraint): tables are firmware-owned binary
 * structures at arbitrary alignment.  Nothing here reads a struct field as a
 * wide load; every integer is assembled byte by byte by acpi_rd*().  The
 * struct below exists only to type the pointer acpi_find_table() hands back.
 */

#include <stdint.h>

/* Standard ACPI SDT header: 36 bytes, shared by RSDT/XSDT/DMAR/MCFG/... .
 * NOT laid out for direct field access -- use acpi_sdt_length()/acpi_sig_eq(). */
struct acpi_sdt_header {
  char signature[4];
  uint32_t length;
  uint8_t revision;
  uint8_t checksum;
  char oem_id[6];
  char oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
};

/* Wire sizes (an SDT header is 36 bytes on every platform; the C struct above
 * only matches that because no field needs padding at its offset). */
#define ACPI_SDT_HEADER_SIZE 36
#define ACPI_RSDP_SIG        "RSD PTR "
#define ACPI_RSDP_SIG_LEN    8
#define ACPI_RSDP_V1_SIZE    20   /* checksum covers this much (ACPI 1.0)     */
#define ACPI_RSDP_V2_SIZE    36   /* extended checksum covers this much      */
/* RSDP lives in the first MiB of physical memory, on a 16-byte boundary. */
#define ACPI_RSDP_SCAN_START 0x00000000ULL
#define ACPI_RSDP_SCAN_END   0x000FFFFFULL
#define ACPI_RSDP_ALIGN      16

/* Locate the RSDP and its RSDT/XSDT root.  Returns 0 when a root table was
 * found and is usable, negative (-1) when no RSDP with a valid checksum
 * exists.  Idempotent: a second call is a no-op once initialised. */
int acpi_init(void);

/* Return the `index`-th table whose 4-byte signature is sig4 (index 0 = the
 * first match), or 0 when there is no such table.  The returned pointer is
 * usable directly (identity map / direct map); the whole table is mapped when
 * a table sits above the direct map. */
const struct acpi_sdt_header *acpi_find_table(const char *sig4, int index);

/* Number of tables the RSDT/XSDT lists (0 before acpi_init()). */
int acpi_table_count(void);

/* Physical address of the RSDP (0 when none was found). */
uint64_t acpi_rsdp_phys(void);

/* --- accessors for a table pointer, byte-wise ---------------------------- */

/* Total table length including the 36-byte header (0 for a NULL pointer). */
uint32_t acpi_sdt_length(const void *sdt);

/* 1 when the table's 4-byte signature equals sig4. */
int acpi_sig_eq(const void *sdt, const char *sig4);

/* --- test seams ---------------------------------------------------------- */

/* Build the table tree from an RSDP found at a known physical address instead
 * of scanning (unit tests point this at a synthetic stack of tables).  Same
 * validation as acpi_init(), including the RSDP checksum. */
int acpi_init_from_root(uint64_t rsdp_phys);

/* Save/restore the discovered state, so a suite that injects a synthetic root
 * can put the boot-time tree back when it is done. */
struct acpi_state {
  uint64_t rsdp_phys;
  uint64_t root_phys;
  uint32_t root_length;
  int root_is_xsdt;
  int ready;
};
void acpi_get_state(struct acpi_state *out);
void acpi_set_state(const struct acpi_state *in);

#endif /* ACPI_H */
