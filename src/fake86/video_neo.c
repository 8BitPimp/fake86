/*
  Fake86: A portable, open-source 8086 PC emulator.
  Copyright (C)2010-2013 Mike Chambers

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

#include "common.h"

#include "../80x86/cpu.h"

// http://www.osdever.net/FreeVGA/vga/vgareg.htm
// http://www.osdever.net/FreeVGA/vga/graphreg.htm#05
// https://wiki.osdev.org/VGA_Hardware#Port_0x3C0

// text mode layout:
//  [[char], [attr]], [[char], [attr]], ...


// cga has 16kb ram at 0xB8000 for framebuffer
// frame buffer is incompletely decoded and is mirrored at 0xBC000
// text mode page is either 2k bytes (40x25x2) or 4k bytes (80x25x2)
#define MAX_PAGES 16

enum system_t {
  video_mda,
  video_cga,
  video_ega,
  video_vga,
};

enum mode_t {
  mode_text,
  mode_graphics,
};

struct cursor_t {
  uint32_t x, y;
  uint8_t size;
};

// cursor per page
static struct cursor_t _cursor[MAX_PAGES];

// current video mode
static uint8_t _video_mode = 0x00;
static enum system_t _system = video_mda;
static enum system_t _mode = mode_text;
// screen resolution
static uint32_t _width = 320, _height = 240;
// text mode rows and columns
static uint32_t _rows = 25, _cols = 40;
// display pages
static uint32_t _pages = 8;
// video memory base
static uint32_t _base = 0xB8000;
//
static uint8_t _active_page = 0;

static bool no_blanking = false;

// 4x 64k memory planes
static uint8_t _vga_ram[0x40000];


// CRTC (6845) address register
static uint8_t crt_reg_addr = 0;

// CRTC (6845) data registers
//
// 0 - horz. total
// 1 - horz. displayed
// 2 - horz. sync pos
// 3 - horz. & vert. sync widths
// 4 - vert. total
// 5 - vert. total adjust
// 6 - vert. displayed
// 7 - vert. sync pos
// 8 - interlace and skew
// 9 - max raster address
// 12 - display start address hi
// 13 - display start address lo
// 14 - cursor address hi
// 15 - cursor address lo
//
static uint8_t crt_register[32];

uint8_t neo_crt_register(uint32_t index) {
  return crt_register[index & 0x1f];
}

uint16_t neo_crt_cursor_reg(void) {
  const uint32_t hi = crt_register[12];
  const uint32_t lo = crt_register[13];
  return 0x3FFF & ((hi << 8) | lo);
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----
// ports 03B0-03BF

// 5 - blinking     (1 enable, 0 disable)
// 3 - video output (1 enable, 0 disable)
// 1 - black and white
// 0 - high res mode
static uint8_t mda_control = 0;

// 3 - 1 if currently drawing something bright
// 0 - horz. retrace (1 true, 0 false)
static uint8_t mda_status = 0;

static uint8_t mda_port_read(uint16_t portnum) {
  if (portnum >= 0x03B0 && portnum <= 0x03B7) {
    if (portnum & 1) {
      return crt_register[crt_reg_addr];
    } else {
      // write only but lets return it anyway
      return crt_reg_addr;
    }
  }
  else {
    if (portnum == 0x03BA) {
      const uint8_t port_3da = vga_timing_get_3da();
      mda_status = (port_3da & 1) ? 1 : 0;
      return mda_status | 0xf0;
    }
  }
  return 0;
}

static void mda_port_write(uint16_t portnum, uint8_t value) {
  if (portnum >= 0x03B0 && portnum <= 0x03B7) {
    if (portnum & 1) {
      crt_register[crt_reg_addr] = value;
    } else {
      crt_reg_addr = value & 0x1f;
    }
  }
  if (portnum == 0x03b8) {
    mda_control = value;
  }
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----
// VGA Sequencer Registers - 3C4 - 3C5

// port 3C4h
static uint8_t _vga_seq_addr;

// port 3C5h
// 
// Index 00h -- Reset Register
// Index 01h -- Clocking Mode Register
// Index 02h -- Map Mask Register
// Index 03h -- Character Map Select Register
// Index 04h -- Sequencer Memory Mode Register
//
static uint8_t _vga_seq_data[256];

// memory plane write enable
// 0x3CE  02  ....**** lsb
//
static uint32_t _vga_plane_write_enable(void) {
  return _vga_seq_data[0x2] & 0x0f;
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----
// VGA Graphics Controller - 3CE - 3CF

// port 3CEh
static uint8_t _vga_reg_addr;

// port 3CFh
//
// Index 00h -- Set/Reset Register
// Index 01h -- Enable Set/Reset Register
// Index 02h -- Color Compare Register
// Index 03h -- Data Rotate Register
// Index 04h -- Read Map Select Register
// Index 05h -- Graphics Mode Register
// Index 06h -- Miscellaneous Graphics Register
// Index 07h -- Color Don't Care Register
// Index 08h -- Bit Mask Register
//
static uint8_t _vga_reg_data[256];

static uint32_t _vga_write_mode(void) {
  return _vga_reg_data[0x5] & 3;
}

static uint32_t _vga_read_mode(void) {
  return (_vga_reg_data[0x5] >> 3) & 1;
}

static uint32_t _vga_read_map_select(void) {
  return _vga_reg_data[0x4] & 3;
}

static uint32_t _vga_memory_map_select(void) {
  return (_vga_reg_data[0x6] >> 2) & 3;
}

// enable set/reset
// 0x3CE  01  ....**** lsb
//
static uint32_t _vga_sr_enable(void) {
  return _vga_reg_data[0x1] & 0x0f;
}

// set/reset value
// 0x3CE  00  ....**** lsb
//
static uint32_t _vga_sr_value(void) {
  return _vga_reg_data[0x0] & 0x0f;
}

// vga alu logical operation
// 0x3CE  03  ...**... lsb
//
static uint32_t _vga_logic_op(void) {
  return (_vga_reg_data[0x3] >> 3) & 0x03;
}

// vga bit rotate count
// 0x3CE  03  .....*** lsb
//
static uint32_t _vga_rot_count(void) {
  return _vga_reg_data[0x3] & 0x07;
}

// vga bit mask register
// 0x3Ce  08  ******** lsb
static uint32_t _vga_bit_mask(void) {
  return _vga_reg_data[0x8];
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----
// VGA DAC - 3C6H - 3C9H

// bit layout
// msb                             lsb
// ________ rrrrrr__ gggggg__ bbbbbb__
static uint32_t _dac_entry[256];

static uint8_t _dac_state;       // dac state, port 0x3c7

// note 8-bit size wraps implicitly
static uint8_t _dac_mode_write;  // dac write address
static uint8_t _dac_mode_read;   // dac read address

// XXX: these may be the same thing?
static uint8_t _dac_pal_read;    // palette index (r, g, b, r, g ...)
static uint8_t _dac_pal_write;   // palette index (r, g, b, r, g ...)

static uint8_t _dac_mask_reg;    // port 0x3c6


const uint32_t *neo_dac_data(void) {
  return _dac_entry;
}

static uint8_t _dac_data_read(void) {
  uint8_t out = 0;
  switch (_dac_pal_read) {
  case 0:
    out = 0x3f & (_dac_entry[_dac_mode_read] >> 18);
    _dac_pal_read = 1;
    break;
  case 1:
    out = 0x3f & (_dac_entry[_dac_mode_read] >> 10);
    _dac_pal_read = 2;
    break;
  case 2:
    out = 0x3f & (_dac_entry[_dac_mode_read] >> 2);
    _dac_pal_read = 0;
    ++_dac_mode_read;
    break;
  }
  return out;
}

static void _dac_data_write(const uint8_t val) {
  switch (_dac_pal_write) {
  case 0:
    _dac_entry[_dac_mode_write] &= 0x00FFFF;
    _dac_entry[_dac_mode_write] |= ((uint32_t)val) << 18;
    _dac_pal_write = 1;
    break;
  case 1:
    _dac_entry[_dac_mode_write] &= 0xFF00FF;
    _dac_entry[_dac_mode_write] |= ((uint32_t)val) << 10;
    _dac_pal_write = 2;
    break;
  case 2:
    _dac_entry[_dac_mode_write] &= 0xFFFF00;
    _dac_entry[_dac_mode_write] |= ((uint32_t)val) << 2;
    _dac_pal_write = 0;
    ++_dac_mode_write;
    break;
  }
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----
// ports 03C0-03CF

static uint8_t ega_port_read(uint16_t portnum) {
  switch (portnum) {
  case 0x3c4:
    return _vga_seq_addr;
  case 0x3c5:
    return _vga_seq_data[_vga_seq_addr];
  case 0x3c6:
    return _dac_mask_reg;
  case 0x3c7:
    return _dac_state & 0x3;
  case 0x3c8:
    // XXX: this is uncertain
    return _dac_mode_write;
  case 0x3c9:
    return _dac_data_read();
  case 0x3ce:
    return _vga_reg_addr;
  case 0x3cf:
    return _vga_reg_data[_vga_reg_addr];
  default:
    return portram[portnum];
  }
}

static void ega_port_write(uint16_t portnum, uint8_t value) {
  switch (portnum) {
//case 0x3c3:
//  video subsystem enable

  case 0x3c4:
    _vga_seq_addr = value;
    break;
  case 0x3c5:
//    printf("_vga_seq_data[0x%02x] = 0x%02x\n", _vga_seq_addr, value);
    _vga_seq_data[_vga_seq_addr] = value;
    break;

  case 0x3c6:
//    printf("_vga_mask_reg = 0x%02x\n", value);
    _dac_mask_reg = value;
    break;

  case 0x3c7:
    _dac_mode_read  = value;
    _dac_pal_read   = 0;
    _dac_state      = 0x00;  // prepared to accept reads
    break;
  case 0x3c8:
    _dac_mode_write = value;
    _dac_pal_write  = 0;
    _dac_state      = 0x03;  // prepared to accept writes
    break;
  case 0x3c9:
    _dac_data_write(value);
    break;

  case 0x3ce:
    _vga_reg_addr = value;
    break;
  case 0x3cf:
//    printf("_vga_reg_data[0x%02x] = 0x%02x\n", _vga_reg_addr, value);
    _vga_reg_data[_vga_reg_addr] = value;
    break;

  default:
    portram[portnum] = value;
  }
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----
// ports 03D0-03DF

static uint8_t cga_control = 0;
static uint8_t cga_palette = 0;

static uint8_t cga_port_read(uint16_t portnum) {

  if (portnum >= 0x03d0 && portnum <= 0x03d7) {
    if (portnum & 1) {
      return crt_register[crt_reg_addr];
    } else {
      return crt_reg_addr;
    }
  }

  switch (portnum) {
  case 0x3d8: // mode control register
    return cga_control;
  case 0x3d9: // colour control register
    return cga_palette;
  case 0x3da:
    return portram[0x3da] = vga_timing_get_3da();
  default:
//    printf("%04x r\n", (int)portnum);
    break;
  }
  return portram[portnum];
}

static void cga_port_write(uint16_t portnum, uint8_t value) {
  if (portnum >= 0x03d0 && portnum <= 0x03d7) {
    if (portnum & 1) {
      crt_register[crt_reg_addr] = value;
    } else {
      crt_reg_addr = value & 0x1f;
    }
  }
  else {
    switch (portnum) {
    case 0x3d8:
      cga_control = value;
      break;
    case 0x3d9:
      cga_palette = value;
      break;
    default:
//      printf("%04x w %02x\n", (int)portnum, (int)value);
      portram[portnum] = value;
    }
  }
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// update neo display adapter
void neo_tick(uint64_t cycles) {
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

static void _clear_text_buffer(void) {

  const uint32_t mem_size = 1024 * 16;

  for (int i=0; i<mem_size; i+=2) {
    RAM[0xB8000 + i + 0] = 0x0;
    RAM[0xB8000 + i + 1] = 0x0;
  }
}

static void neo_set_video_mode(uint8_t al) {

  log_printf(LOG_CHAN_VIDEO, "set video mode to %02Xh", (int)al);

  // check for no blanking
  no_blanking = ((cpu_regs.al & 0x80) != 0);
  al &= 0x7f;

  // text mode columns and rows
  switch (al) {
  case 0x00:
  case 0x01:
  case 0x04:
  case 0x05:
  case 0x0D:
  case 0x13: _cols = 40; _rows = 25; break;
  case 0x02:
  case 0x03:
  case 0x06:
  case 0x07:
  case 0x0E:
  case 0x0F:
  case 0x10: _cols = 80; _rows = 25; break;
  case 0x11:
  case 0x12: _cols = 80; _rows = 30; break;
  }
  // pixel resolution
  switch (al) {
  case 0x04:
  case 0x05:
  case 0x0D:
  case 0x13: _width = 320; _height = 200; break;
  case 0x06:
  case 0x0E: _width = 640; _height = 200; break;
  case 0x0F:
  case 0x10: _width = 640; _height = 350; break;
  case 0x11:
  case 0x12: _width = 640; _height = 480; break;
  }
  // memory base
  if (al >= 0x00 && al <= 0x07) {
    _base = 0xB8000;
//    _clear_text_buffer();
  }
  if (al >= 0x0D && al <= 0x13) {
    _base = 0xA0000;
  }

  _video_mode = al;
}

// set cursor shape
static void do_int10_01(void) {
}

// set cursor position
static void do_int10_02(void) {
  const int page = cpu_regs.bh;
  assert(page <= MAX_PAGES);
  struct cursor_t *c = &_cursor[page];
  c->x = cpu_regs.dl;
  c->y = cpu_regs.dh;
}

// get cursor mode and shape
static void do_int10_03(int page_num) {
  const int page = cpu_regs.bh;
  assert(page <= MAX_PAGES);
  struct cursor_t *c = &_cursor[page];
  cpu_regs.ax = 0;
  cpu_regs.ch = 0; // start scanline
  cpu_regs.cl = 0; // end scanline
  cpu_regs.dh = c->y;
  cpu_regs.dl = c->x;
}

// select active display page 
static void do_int10_05(void) {
  switch (cpu_regs.al) {
  case 0x81: // cpu page regs
  case 0x82: // crt page regs
  case 0x83: // both
    break;
  }
}

// scroll window up
static void do_int10_06(void) {
}

// scroll window down
static void do_int10_07(void) {
}

// read character and attribute at cursor position
static void do_int10_08(void) {
}

// write character and attribute at cursor position 
static void do_int10_09(void) {
}

// write character only at cursor position
static void do_int10_0A(void) {
}

// teletype output
static void do_int10_0E(void) {
}

static void do_int10_0F(void) {
  cpu_regs.ah = _cols;
  cpu_regs.al = _video_mode | (no_blanking ? 0x80 : 0x00);
  cpu_regs.bh = _active_page;
}

// write to dac registers (vga) or Alternate Select (ega)?
static void do_int10_12(void) {
}

// write string (EGA+)
static void do_int10_13(void) {
}

// get/set display combination
static void do_int10_1AXX(void) {
}

static void do_int10_30XX(void) {
  cpu_regs.cx = 0;
  cpu_regs.dx = 0;
}

// BIOS int 10h Video Services handler
bool neo_int10_handler(void) {
  switch (cpu_regs.ah) {
  case 0x00:
    neo_set_video_mode(cpu_regs.al);
    // must return false
    return false;
  }
  return false;
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// initalize neo display adapter
bool neo_init(void) {

  // mda
  set_port_read_redirector(0x3B0, 0x3BF, mda_port_read);
  set_port_write_redirector(0x3B0, 0x3BF, mda_port_write);

  // ega
  set_port_read_redirector(0x3C0, 0x3CF, ega_port_read);
  set_port_write_redirector(0x3C0, 0x3CF, ega_port_write);

  // cga
  set_port_read_redirector(0x3D0, 0x3DF, cga_port_read);
  set_port_write_redirector(0x3D0, 0x3DF, cga_port_write);

  return true;
}

int neo_get_video_mode(void) {
  return _video_mode;
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// four latch bytes packed in 32bits
static uint32_t _vga_latch;

// EGA/VGA
uint8_t neo_mem_read_A0000(uint32_t addr) {
  addr -= 0xA0000;
  const uint32_t planesize = 0x10000;
  switch (_vga_read_mode()) {
  case 0:
    switch (_vga_read_map_select()) {
    case 0: return _vga_ram[addr + planesize * 0];
    case 1: return _vga_ram[addr + planesize * 1];
    case 2: return _vga_ram[addr + planesize * 2];
    case 3: return _vga_ram[addr + planesize * 3];
    }
  case 1:
    return RAM[addr];
    break;
  default:
    UNREACHABLE();
  }
}

static uint8_t _ror8(uint8_t in, uint8_t rot) {
  rot &= 7;
  // rotate right by rot bits (max 7)
  return (in >> rot) | (((uint16_t)in << 8) >> rot);
}

static void _neo_vga_write_0(uint32_t addr, uint8_t value) {

  value = _ror8(value, _vga_rot_count());
  
  // 4 lanes of input bytes
  uint32_t path = (value << 24) | (value << 16) | (value << 8) | value;
  // 4 lanes on input bits from s/r value
  uint32_t srvl = _vga_sr_value() ? 0 : ~0u;

  // mask to mux between bytes or s/r value
  // todo: precompute this
  const uint8_t sr_reg = _vga_sr_enable();
  uint32_t sr_mask;
  sr_mask  = (sr_reg & 0x1) ? 0 : 0x000000ff;
  sr_mask |= (sr_reg & 0x2) ? 0 : 0x0000ff00;
  sr_mask |= (sr_reg & 0x4) ? 0 : 0x00ff0000;
  sr_mask |= (sr_reg & 0x8) ? 0 : 0xff000000;

  // mux between byte inputs or s/r value
  uint32_t tmp0 = (path & sr_mask) | (srvl & ~sr_mask);

  // alu operations
  uint32_t tmp1;
  switch (_vga_logic_op()) {
  case 0: tmp1 = tmp0;              break;
  case 1: tmp1 = tmp0 & _vga_latch; break;
  case 2: tmp1 = tmp0 | _vga_latch; break;
  case 3: tmp1 = tmp0 ^ _vga_latch; break;
  }

  // mux between tmp0 or alu results
  // todo: precompute this
  uint32_t bm_mux;
  bm_mux  = (_vga_bit_mask() & 0x1) ? 0x000000ff : 0;
  bm_mux |= (_vga_bit_mask() & 0x2) ? 0x0000ff00 : 0;
  bm_mux |= (_vga_bit_mask() & 0x4) ? 0x00ff0000 : 0;
  bm_mux |= (_vga_bit_mask() & 0x8) ? 0xff000000 : 0;

  uint32_t tmp2 = (tmp1 & bm_mux) | (_vga_latch & ~bm_mux);

  const uint32_t planesize = 0x10000;

  if (_vga_plane_write_enable() & 0x01) {
    _vga_ram[addr + planesize * 0] = tmp2 & 0xff;
  }
  if (_vga_plane_write_enable() & 0x02) {
    _vga_ram[addr + planesize * 1] = (tmp2 >> 8) & 0xff;
  }
  if (_vga_plane_write_enable() & 0x04) {
    _vga_ram[addr + planesize * 2] = (tmp2 >> 16) & 0xff;
  }
  if (_vga_plane_write_enable() & 0x08) {
    _vga_ram[addr + planesize * 3] = (tmp2 >> 24) & 0xff;
  }

#undef PHASE2
}

static void _neo_vga_write_1(uint32_t addr, uint8_t value) {}

static void _neo_vga_write_2(uint32_t addr, uint8_t value) {}

static void _neo_vga_write_3(uint32_t addr, uint8_t value) {}

// EGA/VGA
void neo_mem_write_A0000(uint32_t addr, uint8_t value) {

  addr -= 0xA0000;

  switch (_vga_write_mode()) {
  case 0: _neo_vga_write_0(addr, value); break;
  case 1: _neo_vga_write_1(addr, value); break;
  case 2: _neo_vga_write_2(addr, value); break;
  case 3: _neo_vga_write_3(addr, value); break;
  default:
    UNREACHABLE();
  }
}

const uint8_t *vga_ram(void) {
  return _vga_ram;
}