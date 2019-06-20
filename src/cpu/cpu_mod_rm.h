/*
  Fake86: A portable, open-source 8086 PC emulator.
  Copyright (C)2010-2013 Mike Chambers
               2019      Aidan Dodds

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
  USA.
*/

#pragma once

#include <stdint.h>


#define GET_CODE(TYPE, OFFSET)                                                \
  (*(const TYPE *)(code + OFFSET))

struct cpu_mod_rm_t {

  uint8_t mod;
  uint8_t reg;
  uint8_t rm;

  uint32_t ea;

  // number of bytes following instruction opcode
  uint8_t num_bytes;
};

// get word register from REG field
static inline uint16_t _get_reg_w(const uint8_t num) {
  switch (num) {
  case 0: return cpu_regs.ax;
  case 1: return cpu_regs.cx;
  case 2: return cpu_regs.dx;
  case 3: return cpu_regs.bx;
  case 4: return cpu_regs.sp;
  case 5: return cpu_regs.bp;
  case 6: return cpu_regs.si;
  case 7: return cpu_regs.di;
  default:
    UNREACHABLE();
  }
}

// get byte register from REG field
static inline uint8_t _get_reg_b(const uint8_t num) {
  switch (num) {
  case 0: return cpu_regs.al;
  case 1: return cpu_regs.cl;
  case 2: return cpu_regs.dl;
  case 3: return cpu_regs.bl;
  case 4: return cpu_regs.ah;
  case 5: return cpu_regs.ch;
  case 6: return cpu_regs.dh;
  case 7: return cpu_regs.bh;
  default:
    UNREACHABLE();
  }
}

// set word register from REG field
static inline void _set_reg_w(const uint8_t num, const uint16_t val) {
  switch (num) {
  case 0: cpu_regs.ax = val; return;
  case 1: cpu_regs.cx = val; return;
  case 2: cpu_regs.dx = val; return;
  case 3: cpu_regs.bx = val; return;
  case 4: cpu_regs.sp = val; return;
  case 5: cpu_regs.bp = val; return;
  case 6: cpu_regs.si = val; return;
  case 7: cpu_regs.di = val; return;
  default:
    UNREACHABLE();
  }
}

// set byte register from REG field
static inline void _set_reg_b(const uint8_t num, const uint8_t val) {
  switch (num) {
  case 0: cpu_regs.al = val; return;
  case 1: cpu_regs.cl = val; return;
  case 2: cpu_regs.dl = val; return;
  case 3: cpu_regs.bl = val; return;
  case 4: cpu_regs.ah = val; return;
  case 5: cpu_regs.ch = val; return;
  case 6: cpu_regs.dh = val; return;
  case 7: cpu_regs.bh = val; return;
  default:
    UNREACHABLE();
  }
}

static inline void _write_rm_b(struct cpu_mod_rm_t *m, const uint8_t v) {
  if (m->mod == 3) {
    _set_reg_b(m->rm, v);
  }
  else {
    write86(m->ea, v);
  }
}

static inline void _write_rm_w(struct cpu_mod_rm_t *m, const uint16_t v) {
  if (m->mod == 3) {
    _set_reg_w(m->rm, v);
  }
  else {
    writew86(m->ea, v);
  }
}

static inline uint8_t _read_rm_b(struct cpu_mod_rm_t *m) {
  return (m->mod == 3) ? _get_reg_b(m->rm) : read86(m->ea);
}

static inline uint16_t _read_rm_w(struct cpu_mod_rm_t *m) {
  return (m->mod == 3) ? _get_reg_w(m->rm) : readw86(m->ea);
}

static inline void _decode_mod_rm(
    const uint8_t *code,
    struct cpu_mod_rm_t *m) {

  // decode mod-reg-rm byte
  const uint8_t modRegRM = GET_CODE(uint8_t, 1);
  m->mod = (modRegRM >> 6) & 0x3;
  m->reg = (modRegRM >> 3) & 0x7;
  m->rm  = (modRegRM >> 0) & 0x7;

  int32_t addr = 0;

  // inital decode of the rm field
  switch (m->mod) {
  case 0:
    if (m->rm == 6) {
      addr = GET_CODE(uint16_t, 2);
      break;
    }
  case 1:
  case 2:

    switch (m->rm) {
    case 0: addr = cpu_regs.bx + cpu_regs.si; break; // [BX + SI]
    case 1: addr = cpu_regs.bx + cpu_regs.di; break; // [BX + DI]
    case 2: addr = cpu_regs.bp + cpu_regs.si; break; // [BP + SI]
    case 3: addr = cpu_regs.bp + cpu_regs.di; break; // [BP + DI]
    case 4: addr =               cpu_regs.si; break; // [SI]
    case 5: addr =               cpu_regs.di; break; // [DI]
    case 6: addr = cpu_regs.bp;               break; // [BP]
    case 7: addr = cpu_regs.bx;               break; // [BX]
    default:
      UNREACHABLE();
    }

    break;
  case 3:
    // treat rm-field as reg-field
    m->num_bytes = 1;
    return;
  default:
    UNREACHABLE();
  }

  uint32_t seg;
  if (m->rm == 2 || m->rm == 3 || (m->rm == 6 && m->mod != 0)) {
    // if we are using cpu_regs.bp as part of the modregrm byte
    seg = cpu_regs.ss << 4;
  }
  else {
    seg = cpu_regs.ds << 4;
  }

  // apply displacements/reg lookup and work out byte count
  switch (m->mod) {
  case 0:
    m->ea = seg + addr;
    if (m->rm == 6) {
      m->num_bytes = 3;
    }
    else {
      m->num_bytes = 1;
    }
    break;
  case 1:
    m->num_bytes = 2;
    m->ea = seg + addr + GET_CODE(int8_t, 2);
    break;
  case 2:
    m->num_bytes = 3;
    m->ea = seg + addr + GET_CODE(uint16_t, 2);
    break;
  default:
    UNREACHABLE();
  }
}
