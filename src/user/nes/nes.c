/*
 * nes.c — compact 6502 + bus + minimal PPU for the HobbyOS NES emulator (GB-035).
 *
 * Single-source, pure C (no HobbyOS dependency) so the host T1 golden drives
 * it directly.  The 6502 core implements the standard instruction set via a
 * decode table; the PPU renders the 32x30 nametable from the 8x8 pattern
 * table into a 256x240 framebuffer.  NROM (mapper-0) cart with 32 KiB PRG.
 *
 * License: MIT (swarm-authored).
 */
#include "nes.h"

/* --- flags ------------------------------------------------------------ */
#define F_N 0x80
#define F_V 0x40
#define F_B 0x10
#define F_D 0x08
#define F_I 0x04
#define F_Z 0x02
#define F_C 0x01

/* --- addressing mode / instruction decode ----------------------------- */
enum {
  AM_IMP, AM_ACC, AM_IMM, AM_ZP, AM_ZPX, AM_ZPY,
  AM_ABS, AM_ABX, AM_ABY, AM_IND, AM_IZX, AM_IZY, AM_REL
};
enum {
  G_NOP, G_LDA, G_LDX, G_LDY, G_STA, G_STX, G_STY, G_TAX, G_TAY, G_TXA,
  G_TYA, G_TSX, G_TXS, G_PHA, G_PHP, G_PLA, G_PLP, G_ADC, G_SBC, G_AND,
  G_ORA, G_EOR, G_CMP, G_CPX, G_CPY, G_ASL, G_LSR, G_ROL, G_ROR, G_INC,
  G_DEC, G_INX, G_INY, G_DEX, G_DEY, G_BPL, G_BMI, G_BVC, G_BVS, G_BCC,
  G_BCS, G_BNE, G_BEQ, G_JMP, G_JSR, G_RTS, G_BRK, G_RTI, G_CLC, G_SEC,
  G_CLI, G_SEI, G_CLV, G_CLD, G_SED
};

/* 256-entry decode: opcode -> (group<<5 | mode). Fits u16 (group<=54, mode<=12). */
static const u16 dec[256] = {
  /*0x00*/ G_BRK<<5|AM_IMP, G_ORA<<5|AM_IZX, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IZX,
  /*0x04*/ G_NOP<<5|AM_ZP,  G_ORA<<5|AM_ZP,  G_ASL<<5|AM_ZP,  G_NOP<<5|AM_IZX,
  /*0x08*/ G_PHP<<5|AM_IMP, G_ORA<<5|AM_IMM, G_ASL<<5|AM_ACC, G_NOP<<5|AM_IMP,
  /*0x0c*/ G_NOP<<5|AM_ABS, G_ORA<<5|AM_ABS, G_ASL<<5|AM_ABS, G_NOP<<5|AM_ABX,
  /*0x10*/ G_BPL<<5|AM_REL, G_ORA<<5|AM_IZY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IZY,
  /*0x14*/ G_NOP<<5|AM_ZPX, G_ORA<<5|AM_ZPX, G_ASL<<5|AM_ZPX, G_NOP<<5|AM_ABY,
  /*0x18*/ G_CLC<<5|AM_IMP, G_ORA<<5|AM_ABY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0x1c*/ G_NOP<<5|AM_ABX, G_ORA<<5|AM_ABX, G_ASL<<5|AM_ABX, G_NOP<<5|AM_IMP,
  /*0x20*/ G_JSR<<5|AM_ABS, G_AND<<5|AM_IZX, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IZX,
  /*0x24*/ G_NOP<<5|AM_ZP,  G_AND<<5|AM_ZP,  G_ROL<<5|AM_ZP,  G_NOP<<5|AM_IZX,
  /*0x28*/ G_PLP<<5|AM_IMP, G_AND<<5|AM_IMM, G_ROL<<5|AM_ACC, G_NOP<<5|AM_IMP,
  /*0x2c*/ G_NOP<<5|AM_ABS, G_AND<<5|AM_ABS, G_ROL<<5|AM_ABS, G_NOP<<5|AM_ABX,
  /*0x30*/ G_BMI<<5|AM_REL, G_AND<<5|AM_IZY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IZY,
  /*0x34*/ G_NOP<<5|AM_ZPX, G_AND<<5|AM_ZPX, G_ROL<<5|AM_ZPX, G_NOP<<5|AM_ABY,
  /*0x38*/ G_SEC<<5|AM_IMP, G_AND<<5|AM_ABY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0x3c*/ G_NOP<<5|AM_ABX, G_AND<<5|AM_ABX, G_ROL<<5|AM_ABX, G_NOP<<5|AM_IMP,
  /*0x40*/ G_RTI<<5|AM_IMP, G_EOR<<5|AM_IZX, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IZX,
  /*0x44*/ G_NOP<<5|AM_ZP,  G_EOR<<5|AM_ZP,  G_LSR<<5|AM_ZP,  G_NOP<<5|AM_IZX,
  /*0x48*/ G_PHA<<5|AM_IMP, G_EOR<<5|AM_IMM, G_LSR<<5|AM_ACC, G_NOP<<5|AM_IMP,
  /*0x4c*/ G_JMP<<5|AM_ABS, G_EOR<<5|AM_ABS, G_LSR<<5|AM_ABS, G_NOP<<5|AM_ABX,
  /*0x50*/ G_BVC<<5|AM_REL, G_EOR<<5|AM_IZY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IZY,
  /*0x54*/ G_NOP<<5|AM_ZPX, G_EOR<<5|AM_ZPX, G_LSR<<5|AM_ZPX, G_NOP<<5|AM_ABY,
  /*0x58*/ G_CLI<<5|AM_IMP, G_EOR<<5|AM_ABY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0x5c*/ G_NOP<<5|AM_ABX, G_EOR<<5|AM_ABX, G_LSR<<5|AM_ABX, G_NOP<<5|AM_IMP,
  /*0x60*/ G_RTS<<5|AM_IMP, G_ADC<<5|AM_IZX, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IZX,
  /*0x64*/ G_NOP<<5|AM_ZP,  G_ADC<<5|AM_ZP,  G_ROR<<5|AM_ZP,  G_NOP<<5|AM_IZX,
  /*0x68*/ G_PLA<<5|AM_IMP, G_ADC<<5|AM_IMM, G_ROR<<5|AM_ACC, G_NOP<<5|AM_IMP,
  /*0x6c*/ G_JMP<<5|AM_IND, G_ADC<<5|AM_ABS, G_ROR<<5|AM_ABS, G_NOP<<5|AM_ABX,
  /*0x70*/ G_BVS<<5|AM_REL, G_ADC<<5|AM_IZY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IZY,
  /*0x74*/ G_NOP<<5|AM_ZPX, G_ADC<<5|AM_ZPX, G_ROR<<5|AM_ZPX, G_NOP<<5|AM_ABY,
  /*0x78*/ G_SEI<<5|AM_IMP, G_ADC<<5|AM_ABY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0x7c*/ G_NOP<<5|AM_ABX, G_ADC<<5|AM_ABX, G_ROR<<5|AM_ABX, G_NOP<<5|AM_IMP,
  /*0x80*/ G_NOP<<5|AM_IMM, G_STA<<5|AM_IZX, G_NOP<<5|AM_IMM, G_NOP<<5|AM_IZX,
  /*0x84*/ G_STY<<5|AM_ZP,  G_STA<<5|AM_ZP,  G_STX<<5|AM_ZP,  G_NOP<<5|AM_IZX,
  /*0x88*/ G_DEY<<5|AM_IMP, G_NOP<<5|AM_IMM, G_TXA<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0x8c*/ G_STY<<5|AM_ABS, G_STA<<5|AM_ABS, G_STX<<5|AM_ABS, G_NOP<<5|AM_ABX,
  /*0x90*/ G_BCC<<5|AM_REL, G_STA<<5|AM_IZY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0x94*/ G_STY<<5|AM_ZPX, G_STA<<5|AM_ZPX, G_STX<<5|AM_ZPY, G_NOP<<5|AM_ABY,
  /*0x98*/ G_TYA<<5|AM_IMP, G_STA<<5|AM_ABY, G_TXS<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0x9c*/ G_NOP<<5|AM_ABX, G_STA<<5|AM_ABX, G_NOP<<5|AM_ABY, G_NOP<<5|AM_IMP,
  /*0xa0*/ G_LDY<<5|AM_IMM, G_LDA<<5|AM_IZX, G_LDX<<5|AM_IMM, G_NOP<<5|AM_IZX,
  /*0xa4*/ G_LDY<<5|AM_ZP,  G_LDA<<5|AM_ZP,  G_LDX<<5|AM_ZP,  G_NOP<<5|AM_IZX,
  /*0xa8*/ G_TAY<<5|AM_IMP, G_LDA<<5|AM_IMM, G_TAX<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0xac*/ G_LDY<<5|AM_ABS, G_LDA<<5|AM_ABS, G_LDX<<5|AM_ABS, G_NOP<<5|AM_ABX,
  /*0xb0*/ G_BCS<<5|AM_REL, G_LDA<<5|AM_IZY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0xb4*/ G_LDY<<5|AM_ZPX, G_LDA<<5|AM_ZPX, G_LDX<<5|AM_ZPY, G_NOP<<5|AM_ABY,
  /*0xb8*/ G_CLV<<5|AM_IMP, G_LDA<<5|AM_ABY, G_TSX<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0xbc*/ G_LDY<<5|AM_ABX, G_LDA<<5|AM_ABX, G_LDX<<5|AM_ABY, G_NOP<<5|AM_IMP,
  /*0xc0*/ G_CPY<<5|AM_IMM, G_CMP<<5|AM_IZX, G_NOP<<5|AM_IMM, G_NOP<<5|AM_IZX,
  /*0xc4*/ G_CPY<<5|AM_ZP,  G_CMP<<5|AM_ZP,  G_DEC<<5|AM_ZP,  G_NOP<<5|AM_IZX,
  /*0xc8*/ G_INY<<5|AM_IMP, G_CMP<<5|AM_IMM, G_DEX<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0xcc*/ G_CPY<<5|AM_ABS, G_CMP<<5|AM_ABS, G_DEC<<5|AM_ABS, G_NOP<<5|AM_ABX,
  /*0xd0*/ G_BNE<<5|AM_REL, G_CMP<<5|AM_IZY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0xd4*/ G_NOP<<5|AM_ZPX, G_CMP<<5|AM_ZPX, G_DEC<<5|AM_ZPX, G_NOP<<5|AM_ABY,
  /*0xd8*/ G_CLD<<5|AM_IMP, G_CMP<<5|AM_ABY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0xdc*/ G_NOP<<5|AM_ABX, G_CMP<<5|AM_ABX, G_DEC<<5|AM_ABX, G_NOP<<5|AM_IMP,
  /*0xe0*/ G_CPX<<5|AM_IMM, G_SBC<<5|AM_IZX, G_NOP<<5|AM_IMM, G_NOP<<5|AM_IZX,
  /*0xe4*/ G_CPX<<5|AM_ZP,  G_SBC<<5|AM_ZP,  G_INC<<5|AM_ZP,  G_NOP<<5|AM_IZX,
  /*0xe8*/ G_INX<<5|AM_IMP, G_SBC<<5|AM_IMM, G_NOP<<5|AM_IMP, G_SBC<<5|AM_IMP,
  /*0xec*/ G_CPX<<5|AM_ABS, G_SBC<<5|AM_ABS, G_INC<<5|AM_ABS, G_NOP<<5|AM_ABX,
  /*0xf0*/ G_BEQ<<5|AM_REL, G_SBC<<5|AM_IZY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0xf4*/ G_NOP<<5|AM_ZPX, G_SBC<<5|AM_ZPX, G_INC<<5|AM_ZPX, G_NOP<<5|AM_ABY,
  /*0xf8*/ G_SED<<5|AM_IMP, G_SBC<<5|AM_ABY, G_NOP<<5|AM_IMP, G_NOP<<5|AM_IMP,
  /*0xfc*/ G_NOP<<5|AM_ABX, G_SBC<<5|AM_ABX, G_INC<<5|AM_ABX, G_NOP<<5|AM_IMP
};

static void set_zn(nes_cpu *c, u8 v){
  c->p &= (u8)~(F_N|F_Z);
  if (v & F_N) c->p |= F_N;
  if (v == 0)  c->p |= F_Z;
}

static void push(nes_machine *m, u8 v){
  nes_write(m, (u16)(0x0100 + m->cpu.sp), v);
  m->cpu.sp--;
}
static u8 pull(nes_machine *m){
  m->cpu.sp++;
  return nes_read(m, (u16)(0x0100 + m->cpu.sp));
}

void nes_reset(nes_machine *m){
  m->cpu.a = m->cpu.x = m->cpu.y = 0;
  m->cpu.sp = 0xFD;
  m->cpu.p = F_I;
  m->cpu.pc = (u16)(nes_read(m, 0xFFFC) | (nes_read(m, 0xFFFD) << 8));
  m->cpu.cycles = 0;
  m->ppu.vram_addr = 0;
  m->ppu.vram_latch = 0;
  m->ppu.status = 0;
  m->joy_shift = 0;
  m->joy_strobe = 0;
}

void nes_joy_strobe(nes_machine *m, u8 val){
  if (m->joy_strobe && !val) m->joy_shift = m->joy;
  m->joy_strobe = val;
}
u8 nes_joy_read(nes_machine *m, int port){
  (void)port;
  u8 bit = (u8)(m->joy_shift & 1);
  m->joy_shift = (u8)(m->joy_shift >> 1);
  return (u8)(bit | 0x40);
}

u8 nes_read(nes_machine *m, u16 addr){
  if (addr < 0x2000) return m->ram[addr & 0x7FF];
  if (addr >= 0x8000) return m->prg[addr & 0x7FFF];
  if (addr < 0x4000) {                      /* PPU regs + $2000 mirrors */
    switch (addr & 7) {
      case 2: {
        u8 s = m->ppu.status;
        m->ppu.status &= (u8)~0x80;          /* reading clears bit 7 */
        return (u8)(s | 0x20);               /* + unused bits */
      }
      case 7: {
        u8 v = m->ppu.vram[m->ppu.vram_addr & (NES_VRAM_SIZE-1)];
        m->ppu.vram_addr = (u16)(m->ppu.vram_addr + (m->ppu.ctrl & 4 ? 32 : 1));
        m->ppu.vram_buf = v;
        return v;
      }
    }
    return 0;
  }
  if (addr == 0x4016 || addr == 0x4017) return nes_joy_read(m, addr & 1);
  return 0;
}

void nes_write(nes_machine *m, u16 addr, u8 val){
  if (addr < 0x2000){ m->ram[addr & 0x7FF] = val; return; }
  if (addr >= 0x8000) return;                /* PRG is RO */
  if (addr < 0x4000) {                       /* PPU regs + mirrors */
    switch (addr & 7) {
      case 0: m->ppu.ctrl = val; return;
      case 1: m->ppu.mask = val; return;
      case 3: m->ppu.oam_addr = val; return;
      case 5:
        m->ppu.vram_latch = (u16)((m->ppu.vram_latch & 0xFF00) | val); return;
      case 6: {                               /* two-stage $2006 latch */
        if (m->ppu.vram_latch & 0x8000) {
          u16 lo = val;
          m->ppu.vram_addr = (u16)(((m->ppu.vram_latch << 8) | lo) & 0x3FFF);
          m->ppu.vram_latch = 0;
        } else {
          m->ppu.vram_latch = (u16)(0x8000 | (val << 8));
        }
        return;
      }
      case 7:
        m->ppu.vram[m->ppu.vram_addr & (NES_VRAM_SIZE-1)] = val;
        m->ppu.vram_addr = (u16)(m->ppu.vram_addr + (m->ppu.ctrl & 4 ? 32 : 1));
        return;
    }
    return;
  }
  if (addr == 0x4016) nes_joy_strobe(m, val);
}

void nes_ppu_render(nes_machine *m){
  const u16 base_nt = 0x2000;
  const u16 base_pt = 0x1000;
  for (int ty = 0; ty < 30; ty++) {
    for (int tx = 0; tx < 32; tx++) {
      u8 tile = m->ppu.vram[(base_nt + ty*32 + tx) & (NES_VRAM_SIZE-1)];
      for (int py = 0; py < 8; py++) {
        u16 pat = (u16)(base_pt + tile*16 + py);
        u8 lo = m->ppu.vram[pat & (NES_VRAM_SIZE-1)];
        u8 hi = m->ppu.vram[(pat+8) & (NES_VRAM_SIZE-1)];
        for (int px = 0; px < 8; px++) {
          u8 pal = (u8)(((lo >> (7-px)) & 1) | (((hi >> (7-px)) & 1) << 1));
          u8 shade = (u8)(pal * 64 + 8);
          m->ppu.fb[(ty*8+py)*NES_SCREEN_W + (tx*8+px)] = shade;
        }
      }
    }
  }
}

/* --- execute ---------------------------------------------------------- */
void nes_tick(nes_machine *m){
  nes_cpu *c = &m->cpu;
  c->cycles++;
  u8 op = nes_read(m, c->pc++);
  u16 sel = dec[op];
  u8 mode = (u8)(sel & 0x1F), grp = (u8)(sel >> 5);
  u16 addr = 0;
  u8  rval = 0;

  /* operand fetch */
  switch (mode) {
    case AM_IMM: rval = nes_read(m, c->pc++); break;
    case AM_ZP:  addr = nes_read(m, c->pc++); rval = nes_read(m, addr); break;
    case AM_ZPX: addr = (u8)(nes_read(m, c->pc++) + c->x); rval = nes_read(m, addr); break;
    case AM_ZPY: addr = (u8)(nes_read(m, c->pc++) + c->y); rval = nes_read(m, addr); break;
    case AM_ABS: {
      u8 lo = nes_read(m, c->pc++), hi = nes_read(m, c->pc++);
      addr = (u16)(lo | (hi << 8)); rval = nes_read(m, addr); break;
    }
    case AM_ABX: case AM_ABY: {
      u8 lo = nes_read(m, c->pc++), hi = nes_read(m, c->pc++);
      addr = (u16)(lo | (hi << 8));
      addr = (u16)(addr + (mode == AM_ABX ? c->x : c->y));
      rval = nes_read(m, addr); break;
    }
    case AM_IND: {
      u8 lo = nes_read(m, c->pc++), hi = nes_read(m, c->pc++);
      u16 ptr = (u16)(lo | (hi << 8));
      /* bug-compatible page wrap on low byte */
      u16 p = (u16)((ptr & 0xFF00) | ((ptr + 1) & 0xFF));
      addr = (u16)(nes_read(m, ptr) | (nes_read(m, p) << 8));
      rval = nes_read(m, addr); break;
    }
    case AM_IZX: {
      u8 zp = (u8)(nes_read(m, c->pc++) + c->x);
      addr = (u16)(nes_read(m, (u16)zp) | (nes_read(m, (u16)((zp+1)&0xFF)) << 8));
      rval = nes_read(m, addr); break;
    }
    case AM_IZY: {
      u8 zp = nes_read(m, c->pc++);
      addr = (u16)(nes_read(m, (u16)zp) | (nes_read(m, (u16)((zp+1)&0xFF)) << 8));
      addr = (u16)(addr + c->y); rval = nes_read(m, addr); break;
    }
    case AM_REL: { s16 off = (s16)(int8_t)nes_read(m, c->pc++);
                   addr = (u16)(c->pc + off); break; }
    default: break; /* IMP / ACC */
  }

  int branch_taken = 0;
  switch (grp) {
    case G_LDA: c->a = rval; set_zn(c, c->a); break;
    case G_LDX: c->x = rval; set_zn(c, c->x); break;
    case G_LDY: c->y = rval; set_zn(c, c->y); break;
    case G_STA: nes_write(m, addr, c->a); break;
    case G_STX: nes_write(m, addr, c->x); break;
    case G_STY: nes_write(m, addr, c->y); break;
    case G_TAX: c->x = c->a; set_zn(c, c->x); break;
    case G_TAY: c->y = c->a; set_zn(c, c->y); break;
    case G_TXA: c->a = c->x; set_zn(c, c->a); break;
    case G_TYA: c->a = c->y; set_zn(c, c->a); break;
    case G_TSX: c->x = c->sp; set_zn(c, c->x); break;
    case G_TXS: c->sp = c->x; break;
    case G_PHA: push(m, c->a); break;
    case G_PLA: c->a = pull(m); set_zn(c, c->a); break;
    case G_PHP: push(m, (u8)(c->p | F_B)); break;
    case G_PLP: c->p = (u8)(pull(m) & ~F_B); c->p |= F_B; break;
    case G_AND: c->a = (u8)(c->a & rval); set_zn(c, c->a); break;
    case G_ORA: c->a = (u8)(c->a | rval); set_zn(c, c->a); break;
    case G_EOR: c->a = (u8)(c->a ^ rval); set_zn(c, c->a); break;
    case G_ADC: {
      u8 v = rval, in = c->a;
      unsigned res = in + v + (c->p & F_C ? 1u : 0u);
      c->p = (u8)((c->p & ~(F_C|F_V|F_Z|F_N)) |
                  ((res > 0xFF) ? F_C : 0));
      c->p = (u8)((c->p & ~F_V) | (((in ^ v) & 0x80) ? 0 :
                 ((in ^ (u8)res) & 0x80) ? F_V : 0));
      c->a = (u8)res; set_zn(c, c->a); break;
    }
    case G_SBC: {
      u8 v = (u8)~rval, in = c->a;
      unsigned res = in + v + (c->p & F_C ? 1u : 0u);
      c->p = (u8)((c->p & ~(F_C|F_V|F_Z|F_N)) |
                  ((res > 0xFF) ? F_C : 0));
      c->p = (u8)((c->p & ~F_V) | (((in ^ v) & 0x80) ? 0 :
                 ((in ^ (u8)res) & 0x80) ? F_V : 0));
      c->a = (u8)res; set_zn(c, c->a); break;
    }
    case G_CMP: case G_CPX: case G_CPY: {
      u8 r = (grp == G_CPX) ? c->x : (grp == G_CPY) ? c->y : c->a;
      u8 d = (u8)(r - rval);
      c->p = (u8)((c->p & ~(F_C|F_Z|F_N)) | (r >= rval ? F_C : 0));
      set_zn(c, d); break;
    }
    case G_ASL: case G_LSR: case G_ROL: case G_ROR: {
      u8 val = (mode == AM_ACC) ? c->a : rval;
      u8 carry = (u8)(c->p & F_C);
      u8 out = 0, newc = 0;
      if (grp == G_ASL) { newc = (u8)((val & 0x80) ? F_C : 0); out = (u8)(val << 1); }
      if (grp == G_LSR) { newc = (u8)((val & 1) ? F_C : 0); out = (u8)(val >> 1); }
      if (grp == G_ROL) { newc = (u8)((val & 0x80) ? F_C : 0); out = (u8)((val << 1) | (carry?1:0)); }
      if (grp == G_ROR) { newc = (u8)((val & 1) ? F_C : 0); out = (u8)((val >> 1) | (carry?0x80:0)); }
      c->p = (u8)((c->p & ~(F_C|F_Z|F_N)) | newc);
      set_zn(c, out);
      if (mode == AM_ACC) c->a = out; else nes_write(m, addr, out);
      break;
    }
    case G_INC: { u8 v = (u8)(rval + 1); nes_write(m, addr, v); set_zn(c, v); break; }
    case G_DEC: { u8 v = (u8)(rval - 1); nes_write(m, addr, v); set_zn(c, v); break; }
    case G_INX: c->x++; set_zn(c, c->x); break;
    case G_INY: c->y++; set_zn(c, c->y); break;
    case G_DEX: c->x--; set_zn(c, c->x); break;
    case G_DEY: c->y--; set_zn(c, c->y); break;
    case G_CLC: c->p &= (u8)~F_C; break;
    case G_SEC: c->p |= F_C; break;
    case G_CLI: c->p &= (u8)~F_I; break;
    case G_SEI: c->p |= F_I; break;
    case G_CLV: c->p &= (u8)~F_V; break;
    case G_CLD: c->p &= (u8)~F_D; break;
    case G_SED: c->p |= F_D; break;
    case G_BPL: branch_taken = !(c->p & F_N); break;
    case G_BMI: branch_taken =  (c->p & F_N); break;
    case G_BVC: branch_taken = !(c->p & F_V); break;
    case G_BVS: branch_taken =  (c->p & F_V); break;
    case G_BCC: branch_taken = !(c->p & F_C); break;
    case G_BCS: branch_taken =  (c->p & F_C); break;
    case G_BNE: branch_taken = !(c->p & F_Z); break;
    case G_BEQ: branch_taken =  (c->p & F_Z); break;
    case G_JMP: c->pc = addr; break;
    case G_JSR:
      { u16 ret = (u16)(c->pc - 1);
        push(m, (u8)(ret >> 8)); push(m, (u8)(ret & 0xFF));
        c->pc = addr; break; }
    case G_RTS:
      { u8 lo = pull(m), hi = pull(m);
        c->pc = (u16)(lo | (hi << 8)); c->pc++; break; }
    case G_BRK:
      { u16 ret = (u16)(c->pc + 1);
        push(m, (u8)(ret >> 8)); push(m, (u8)(ret & 0xFF));
        push(m, (u8)(c->p | F_B));
        c->pc = (u16)(nes_read(m, 0xFFFE) | (nes_read(m, 0xFFFF) << 8)); break; }
    case G_RTI:
      { c->p = (u8)(pull(m) & ~F_B); c->p |= F_B;
        u8 lo = pull(m), hi = pull(m);
        c->pc = (u16)(lo | (hi << 8)); break; }
    default: break; /* NOP */
  }
  if (branch_taken) { c->pc = addr; c->cycles++; }
}

void nes_run(nes_machine *m, u16 max_cycles){
  u16 start = m->cpu.cycles;
  while ((u16)(m->cpu.cycles - start) < max_cycles)
    nes_tick(m);
}
