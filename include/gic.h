#ifndef GIC_H
#define GIC_H

#include <stdint.h>

void gic_init(void);
void gic_enable_interrupt(uint32_t intid);
uint32_t gic_acknowledge_interrupt(void);
void gic_end_interrupt(uint32_t intid);

#endif
