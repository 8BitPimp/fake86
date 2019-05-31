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

// References:
//   http://www.osdever.net/FreeVGA/vga/vgareg.htm
//   http://www.osdever.net/FreeVGA/vga/graphreg.htm#05
//   https://wiki.osdev.org/VGA_Hardware#Port_0x3C0
//   https://www.phatcode.net/res/224/files/html/ch27/27-01.html

// Text mode layout:
//  [[char], [attr]], [[char], [attr]], ...

// Misc Notes:
//  cga has 16kb ram at 0xB8000 for framebuffer
//  frame buffer is incompletely decoded and is mirrored at 0xBC000
//  text mode page is either 2k bytes (40x25x2) or 4k bytes (80x25x2)
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

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

// rotate right
static inline uint8_t _ror8(uint8_t in, uint8_t rot) {
  rot &= 7;
  // rotate right by rot bits (max 7)
  return (in >> rot) | (((uint16_t)in << 8) >> rot);
}

// lower four bits to byte mask
static uint32_t _make_mask(const uint8_t bits) {
  uint32_t out;
  out  = (bits & 0x1) ? 0x000000ff : 0;
  out |= (bits & 0x2) ? 0x0000ff00 : 0;
  out |= (bits & 0x4) ? 0x00ff0000 : 0;
  out |= (bits & 0x8) ? 0xff000000 : 0;
  return out;
}

// ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----

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

extern uint8_t _3c0_flipflop;

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

      // reading from 3bA/3dA will set the 3c0 flipflop to index mode
      _3c0_flipflop = 0;

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


const uint32_t *neo_vga_dac(void) {
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

// 
uint32_t _ega_dac[16];
uint8_t _ega_reg[32];

// 0 = index mode
// 1 = value mode
uint8_t _3c0_flipflop;

// port 3c0 address
uint8_t _3c0_addr;

const uint32_t *neo_ega_dac(void) {
  return _ega_dac;
}

static uint32_t _ega_attr_to_rgb(const uint8_t value) {
  // `value` layout:   [msb]  ..rgbRGB  [lsb]
  //
  //                primary                 secondary
  const uint8_t r = ((value >> 4) & 2) | ((value >> 2) & 1);
  const uint8_t g = ((value >> 3) & 2) | ((value >> 1) & 1);
  const uint8_t b = ((value >> 2) & 2) | ((value >> 0) & 1);
  // partial lookup table
  static const uint8_t lut[] = {0x00, 0xaa, 0x55, 0xff};
  // pack as RGB byte
  return (((uint32_t)lut[r]) << 16) |
         (((uint32_t)lut[g]) <<  8) |
         (((uint32_t)lut[b]));
}

static void _write_port_3c0(const uint8_t value) {
  // write mode
  if (_3c0_flipflop & 1) {
    // if this is a palette write
    if (_3c0_addr < 16) {
      _ega_dac[_3c0_addr] = _ega_attr_to_rgb(value);
    }
    // other register
    else {
      _ega_reg[_3c0_addr] = value;
    }
  }
  // set address mode
  else {
    _3c0_addr = value & 0x1f;
  }
  _3c0_flipflop ^= 1;
}

static uint8_t ega_port_read(uint16_t portnum) {
  switch (portnum) {
  case 0x3c0:
    return _3c0_addr;
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
  case 0x3c0:
    _write_port_3c0(value);
    break;

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
    // reading from port 3ba/3da will reset the 3c0 address/data flip flop
    _3c0_flipflop = 0;
    // compute the new vga timing info
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

// Read Mode 0
//
// During a CPU read from the frame buffer, the value returned to the CPU is
// data from the memory plane selected by bits 1 and 0 of the Read Plane Select
// Register (GR04).
static uint8_t _neo_vga_read_0(uint32_t addr) {
  switch (_vga_read_map_select()) {
  case 0: return (_vga_latch >>  0) & 0xff;
  case 1: return (_vga_latch >>  8) & 0xff;
  case 2: return (_vga_latch >> 16) & 0xff;
  case 3: return (_vga_latch >> 24) & 0xff;
  default:
    UNREACHABLE();
  }
}

// Read Mode 1
// 
// During a CPU read from the frame buffer, all 8 bits of the byte in each of
// the 4 memory planes corresponding to the address from which a CPU read
// access is being performed are compared to the corresponding bits in this
// register (if the corresponding bit in the Color Don’t Care Register (GR07)
// is set to 1). The value that the CPU receives from the read access is an
// 8-bit value that shows the result of this comparison. A value of 1 in a
// given bit position indicates that all of the corresponding bits in the bytes
// across all 4 of the memory planes that were included in the comparison had
// the same value as their memory plane’s respective bits in this register.
static uint8_t _neo_vga_read_1(uint32_t addr) {
  // https://www.phatcode.net/res/224/files/html/ch28/28-03.html#Heading4

  //XXX: needed by CIV when it gets to the menu screen
  UNREACHABLE();
}

// EGA/VGA
uint8_t neo_mem_read_A0000(uint32_t addr) {
  addr -= 0xA0000;
  const uint32_t planesize = 0x10000;
  // fill the latches
  _vga_latch  = ((uint32_t)_vga_ram[addr + planesize * 0]) << 0;
  _vga_latch |= ((uint32_t)_vga_ram[addr + planesize * 1]) << 8;
  _vga_latch |= ((uint32_t)_vga_ram[addr + planesize * 2]) << 16;
  _vga_latch |= ((uint32_t)_vga_ram[addr + planesize * 3]) << 24;
  // dispatch via read mode
  switch (_vga_read_mode()) {
  case 0: return _neo_vga_read_0(addr);
  case 1: return _neo_vga_read_1(addr);
  default:
    UNREACHABLE();
  }
}

static void _neo_vga_write_planes(uint32_t addr, const uint32_t lanes) {

  const uint32_t planesize = 0x10000;

  if (_vga_plane_write_enable() & 0x01) {
    _vga_ram[addr + planesize * 0] = (lanes >> 0) & 0xff;
  }
  if (_vga_plane_write_enable() & 0x02) {
    _vga_ram[addr + planesize * 1] = (lanes >> 8) & 0xff;
  }
  if (_vga_plane_write_enable() & 0x04) {
    _vga_ram[addr + planesize * 2] = (lanes >> 16) & 0xff;
  }
  if (_vga_plane_write_enable() & 0x08) {
    _vga_ram[addr + planesize * 3] = (lanes >> 24) & 0xff;
  }
}

static uint32_t _broadcast(const uint8_t val) {
  return (val << 24) | (val << 16) | (val << 8) | val;
}

static void _neo_vga_write_alu(uint32_t addr, uint32_t input) {
    // alu operations
  uint32_t tmp1;
  switch (_vga_logic_op()) {
  case 0: tmp1 = input;              break;
  case 1: tmp1 = input & _vga_latch; break;
  case 2: tmp1 = input | _vga_latch; break;
  case 3: tmp1 = input ^ _vga_latch; break;
  default:
    UNREACHABLE();
  }

  // mux between tmp0 or alu results
  // todo: precompute this
//  const uint32_t bm_mux = _make_mask(_vga_bit_mask());
  const uint32_t bm_mux = _broadcast(_vga_bit_mask());
  uint32_t tmp2 = (tmp1 & bm_mux) | (_vga_latch & ~bm_mux);

  // write data to planes
  _neo_vga_write_planes(addr, tmp2);
}

// 00 = Write Mode 0
//
// During a CPU write to the frame buffer, the addressed byte in each of the 4
// memory planes is written with the CPU write data after it has been rotated
// by the number of counts specified in the Data Rotate Register (GR03). If,
// however, the bit(s) in the Enable Set/Reset Register (GR01) corresponding to
// one or more of the memory planes is set to 1, then those memory planes will
// be written to with the data stored in the corresponding bits in the Set/Reset
// Register (GR00).
static void _neo_vga_write_0(uint32_t addr, uint8_t value) {

  value = _ror8(value, _vga_rot_count());
  
  // 4 lanes of input bytes
  uint32_t path = (value << 24) | (value << 16) | (value << 8) | value;
  // 4 lanes on input bits from s/r value
  uint32_t srvl = _vga_sr_value() ? 0 : ~0u;

  // mask to mux between bytes or s/r value
  // todo: precompute this
  const uint8_t sr_reg = _vga_sr_enable();
  uint32_t sr_mask = ~_make_mask(sr_reg);

  // mux between byte inputs or s/r value
  uint32_t tmp0 = (path & sr_mask) | (srvl & ~sr_mask);

  _neo_vga_write_alu(addr, tmp0);
}

// 01 = Write Mode 1
//
// During a CPU write to the frame buffer, the addressed byte in each of the 4
// memory planes is written to with the data stored in the memory read latches.
// (The memory read latches stores an unaltered copy of the data last read from
// any location in the frame buffer.)
static void _neo_vga_write_1(uint32_t addr, uint8_t value) {
  //XXX: called by INDY

  // untested!
  _neo_vga_write_planes(addr, _vga_latch);
}

// 10 = Write Mode 2
//
// During a CPU write to the frame buffer, the least significant 4 data bits of
// the CPU write data is treated as the color value for the pixels in the
// addressed byte in all 4 memory planes. The 8 bits of the Bit Mask Register
// (GR08) are used to selectively enable or disable the ability to write to the
// corresponding bit in each of the 4 memory planes that correspond to a given
// pixel. A setting of 0 in a bit in the Bit Mask Register at a given bit
// position causes the bits in the corresponding bit positions in the addressed
// byte in all 4 memory planes to be written with value of their counterparts in
// the memory read latches. A setting of 1 in a Bit Mask Register at a given bit
// position causes the bits in the corresponding bit positions in the addressed
// byte in all 4 memory planes to be written with the 4 bits taken from the CPU
// write data to thereby cause the pixel corresponding to these bits to be set
// to the color value.
static void _neo_vga_write_2(uint32_t addr, uint8_t value) {
  //see: https://www.phatcode.net/res/224/files/html/ch27/27-01.html

  //XXX: called by INDY

  const uint32_t mask = _make_mask(value);
  _neo_vga_write_alu(addr, mask);
}

// 11 = Write Mode 3
//
// During a CPU write to the frame buffer, the CPU write data is logically
// ANDed with the contents of the Bit Mask Register (GR08). The result of this
// ANDing is treated as the bit mask used in writing the contents of the
// Set/Reset Register (GR00) are written to addressed byte in all 4 memory
// planes.
static void _neo_vga_write_3(uint32_t addr, uint8_t value) {

  // https://wiki.osdev.org/VGA_Hardware - write mode 3

  // rotate input bits
  value = _ror8(value, _vga_rot_count());

  //XXX: not just be AND, use function select register bits 3-4 for func
  //see: https://cs.nyu.edu/~yap/classes/machineOrg/info/video.htm

  // The resulting value is ANDed with the Bit Mask Register, resulting in the
  // bit mask to be applied
  uint8_t tmp0 = _vga_bit_mask() & value;

  // Each plane takes one bit from the Set/Reset Value register, and turns it
  // into either 0x00 (if set) or 0xff (if clear) 
  uint32_t srvl = _vga_sr_value() ? 0 : ~0u;

  // The computed bit mask is checked, for each set bit the corresponding bit
  // from the set/reset logic is forwarded. If the bit is clear the bit is taken
  // directly from the Latch
  uint32_t switcher = _make_mask(tmp0);
  uint32_t tmp1 = (srvl & switcher) | (_vga_latch & ~switcher );

  // The result is sent towards memory
  _neo_vga_write_planes(addr, tmp1);
}

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
