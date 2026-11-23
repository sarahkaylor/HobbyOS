/* nvdump.c -- read-only BAR0 register dumper for the reference lane.
 * Never writes. 4-byte aligned reads only. Prints "OFFSET VALUE" lines.
 *
 * usage: nvdump named            # the named offset set
 *        nvdump range START END  # [START,END) step 4, one line per register
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>

#define BAR0_LEN (16u * 1024u * 1024u)

static volatile uint32_t *bar;

static void show(uint32_t off)
{
    printf("0x%06X ", off);
    fflush(stdout);
    printf("0x%08X\n", bar[off / 4]);
    fflush(stdout);
}

struct named { uint32_t off; const char *tag; };

static const struct named singles[] = {
    { 0x000000, "PMC_BOOT_0" },
    { 0x000004, "PMC_BOOT_1" },
    { 0x000008, "PMC_BOOT_2" },
    { 0x000100, "PMC_INTR_0" },
    { 0x000140, "PMC_INTR_EN_0" },
    { 0x000200, "PMC_ENABLE" },
    { 0x001668, "MASK_0x1668" },
    { 0x009400, "BUS_009400" },
    { 0x009410, "BUS_009410" },
    { 0x100100, "WIN_100100" },
    { 0x100140, "WIN_100140" },
    { 0x1103C0, "GSP_0x3C0" },
    { 0x110804, "GSP_0x804" },
    { 0x110C00, "GSP_QUEUE_HEAD0" },
    { 0x111668, "RISCV_0x668" },
    { 0x1FA7C8, "PFB_1FA7C8" },
    { 0x1FA824, "WPR2_LO" },
    { 0x1FA828, "WPR2_HI" },
    { 0x1FA82C, "WPR2_1FA82C" },
    { 0x1FA830, "WPR2_1FA830" },
};

struct rng { uint32_t start, end; const char *tag; };

static const struct rng ranges[] = {
    { 0x10A000, 0x10A044, "PMU" },
    { 0x110000, 0x110104, "GSP" },
    { 0x111000, 0x111404, "RISCV" },
    { 0x1FA800, 0x1FA844, "PFB_WPR2" },
    { 0x840000, 0x840044, "WIN840000" },
};

int main(int argc, char **argv)
{
    int fd = open("/sys/bus/pci/devices/0000:01:00.0/resource0", O_RDONLY | O_SYNC);
    if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); return 1; }
    void *m = mmap(NULL, BAR0_LEN, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { fprintf(stderr, "mmap: %s\n", strerror(errno)); return 1; }
    bar = (volatile uint32_t *)m;

    if (argc >= 4 && strcmp(argv[1], "range") == 0) {
        uint32_t s = (uint32_t)strtoul(argv[2], NULL, 0);
        uint32_t e = (uint32_t)strtoul(argv[3], NULL, 0);
        for (uint32_t o = s; o < e; o += 4) show(o);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "one") == 0) {
        show((uint32_t)strtoul(argv[2], NULL, 0));
        return 0;
    }

    size_t i;
    printf("# named single registers\n");
    for (i = 0; i < sizeof(singles)/sizeof(singles[0]); i++) {
        printf("# %s\n", singles[i].tag);
        show(singles[i].off);
    }
    for (i = 0; i < sizeof(ranges)/sizeof(ranges[0]); i++) {
        printf("# range %s 0x%06X..0x%06X\n", ranges[i].tag, ranges[i].start, ranges[i].end);
        for (uint32_t o = ranges[i].start; o < ranges[i].end; o += 4) show(o);
    }
    return 0;
}
