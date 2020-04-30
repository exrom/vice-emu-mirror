/*
 * c64mem.c -- C64 memory handling.
 *
 * Written by
 *  Andreas Boose <viceteam@t-online.de>
 *  Ettore Perazzoli <ettore@comm2000.it>
 *  Marco van den Heuvel <blackystardust68@yahoo.com>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm.h"
#include "c64.h"
#include "c64-memory-hacks.h"
#include "c64-resources.h"
#include "c64_256k.h"
#include "c64cart.h"
#include "c64cia.h"
#include "c64mem.h"
#include "c64meminit.h"
#include "c64memlimit.h"
#include "c64memrom.h"
#include "c64pla.h"
#include "c64ui.h"
#include "c64cartmem.h"
#include "cartio.h"
#include "cartridge.h"
#include "cia.h"
#include "clkguard.h"
#include "machine.h"
#include "maincpu.h"
#include "mem.h"
#include "monitor.h"
#include "plus256k.h"
#include "plus60k.h"
#include "ram.h"
#include "resources.h"
#include "reu.h"
#include "sid.h"
#include "tpi.h"
#include "vicii-mem.h"
#include "vicii-phi1.h"
#include "vicii.h"

/* Machine class (moved from c64.c to distinguish between x64 and x64sc) */
int machine_class = VICE_MACHINE_C64;

/* C64 memory-related resources.  */

/* ------------------------------------------------------------------------- */

/* Number of possible memory configurations.  */
#define NUM_CONFIGS     32

/* Number of possible video banks (16K each).  */
#define NUM_VBANKS      4

/* The C64 memory.  */
uint8_t mem_ram[C64_RAM_SIZE];

#ifdef USE_EMBEDDED
#include "c64chargen.h"
#else
uint8_t mem_chargen_rom[C64_CHARGEN_ROM_SIZE];
#endif

/* Internal color memory.  */
static uint8_t mem_color_ram[0x400];
uint8_t *mem_color_ram_cpu, *mem_color_ram_vicii;

/* Pointer to the chargen ROM.  */
uint8_t *mem_chargen_rom_ptr;

/* Pointers to the currently used memory read and write tables.  */
read_func_ptr_t *_mem_read_tab_ptr;
store_func_ptr_t *_mem_write_tab_ptr;
read_func_ptr_t *_mem_read_tab_ptr_dummy;
store_func_ptr_t *_mem_write_tab_ptr_dummy;
static uint8_t **_mem_read_base_tab_ptr;
static uint32_t *mem_read_limit_tab_ptr;

/* Memory read and write tables.  */
static store_func_ptr_t mem_write_tab[NUM_VBANKS][NUM_CONFIGS][0x101];
static read_func_ptr_t mem_read_tab[NUM_CONFIGS][0x101];
static uint8_t *mem_read_base_tab[NUM_CONFIGS][0x101];
static uint32_t mem_read_limit_tab[NUM_CONFIGS][0x101];

static store_func_ptr_t mem_write_tab_watch[0x101];
static read_func_ptr_t mem_read_tab_watch[0x101];

/* Current video bank (0, 1, 2 or 3).  */
static int vbank;

/* Current memory configuration.  */
static int mem_config;

/* Tape sense status: 1 = some button pressed, 0 = no buttons pressed.  */
static int tape_sense = 0;

static int tape_write_in = 0;
static int tape_motor_in = 0;

/* Current watchpoint state. 
          0 = no watchpoints
    bit0; 1 = watchpoints active
    bit1; 2 = watchpoints trigger on dummy accesses
*/
static int watchpoints_active = 0;

/* ------------------------------------------------------------------------- */

static uint8_t zero_read_watch(uint16_t addr)
{
    addr &= 0xff;
    monitor_watch_push_load_addr(addr, e_comp_space);
    return mem_read_tab[mem_config][0](addr);
}

static void zero_store_watch(uint16_t addr, uint8_t value)
{
    addr &= 0xff;
    monitor_watch_push_store_addr(addr, e_comp_space);
    mem_write_tab[vbank][mem_config][0](addr, value);
}

static uint8_t read_watch(uint16_t addr)
{
    monitor_watch_push_load_addr(addr, e_comp_space);
    return mem_read_tab[mem_config][addr >> 8](addr);
}

static void store_watch(uint16_t addr, uint8_t value)
{
    monitor_watch_push_store_addr(addr, e_comp_space);
    mem_write_tab[vbank][mem_config][addr >> 8](addr, value);
}

/* called by mem_pla_config_changed(), mem_toggle_watchpoints() */
static void mem_update_tab_ptrs(int flag)
{
    if (flag) {
        _mem_read_tab_ptr = mem_read_tab_watch;
        _mem_write_tab_ptr = mem_write_tab_watch;
        if (flag > 1) {
            /* enable watchpoints on dummy accesses */
            _mem_read_tab_ptr_dummy = mem_read_tab_watch;
            _mem_write_tab_ptr_dummy = mem_write_tab_watch;
        } else {
            _mem_read_tab_ptr_dummy = mem_read_tab[mem_config];
            _mem_write_tab_ptr_dummy = mem_write_tab[vbank][mem_config];
        }
    } else {
        /* all watchpoints disabled */
        _mem_read_tab_ptr = mem_read_tab[mem_config];
        _mem_write_tab_ptr = mem_write_tab[vbank][mem_config];
        _mem_read_tab_ptr_dummy = mem_read_tab[mem_config];
        _mem_write_tab_ptr_dummy = mem_write_tab[vbank][mem_config];
    }
}

void mem_toggle_watchpoints(int flag, void *context)
{
    mem_update_tab_ptrs(flag);
    watchpoints_active = flag;
}

/* ------------------------------------------------------------------------- */

/* $00/$01 unused bits emulation

   - There are 2 different unused bits, 1) the output bits, 2) the input bits
   - The output bits can be (re)set when the data-direction is set to output
     for those bits and the output bits will not drop-off to 0.
   - When the data-direction for the unused bits is set to output then the
     unused input bits can be (re)set by writing to them, when set to 1 the
     drop-off timer will start which will cause the unused input bits to drop
     down to 0 in a certain amount of time.
   - When an unused input bit already had the drop-off timer running, and is
     set to 1 again, the drop-off timer will restart.
   - when a an unused bit changes from output to input, and the current output
     bit is 1, the drop-off timer will restart again

    see testprogs/CPU/cpuport for details and tests
*/

static void clk_overflow_callback(CLOCK sub, void *unused_data)
{
    if (pport.data_set_clk_bit6 > (CLOCK)0) {
        pport.data_set_clk_bit6 -= sub;
    }
    if (pport.data_falloff_bit6 && (pport.data_set_clk_bit6 < maincpu_clk)) {
        pport.data_falloff_bit6 = 0;
        pport.data_set_bit6 = 0;
    }
    if (pport.data_set_clk_bit7 > (CLOCK)0) {
        pport.data_set_clk_bit7 -= sub;
    }
    if (pport.data_falloff_bit7 && (pport.data_set_clk_bit7 < maincpu_clk)) {
        pport.data_falloff_bit7 = 0;
        pport.data_set_bit7 = 0;
    }
}

void c64_mem_init(void)
{
    clk_guard_add_callback(maincpu_clk_guard, clk_overflow_callback, NULL);
}

void mem_pla_config_changed(void)
{
    mem_config = (((~pport.dir | pport.data) & 0x7) | (export.exrom << 3) | (export.game << 4));

    c64pla_config_changed(tape_sense, tape_write_in, tape_motor_in, 1, 0x17);

    mem_update_tab_ptrs(watchpoints_active);

    _mem_read_base_tab_ptr = mem_read_base_tab[mem_config];
    mem_read_limit_tab_ptr = mem_read_limit_tab[mem_config];

    maincpu_resync_limits();
}

uint8_t zero_read(uint16_t addr)
{
    uint8_t retval;

    addr &= 0xff;
#ifdef FEATURE_CPUMEMHISTORY
    if (!(memmap_state & MEMMAP_STATE_IGNORE)) {
        monitor_memmap_store(addr, (memmap_state & MEMMAP_STATE_OPCODE) ? MEMMAP_RAM_X : (memmap_state & MEMMAP_STATE_INSTR) ? 0 : MEMMAP_RAM_R);
        memmap_state &= ~(MEMMAP_STATE_OPCODE);
    }
#endif
    switch ((uint8_t)addr) {
        case 0:
            return pport.dir_read;
        case 1:
            retval = pport.data_read;

            /* discharge the "capacitor" */

            /* set real value of read bit 6 */
            if (pport.data_falloff_bit6 && (pport.data_set_clk_bit6 < maincpu_clk)) {
                pport.data_falloff_bit6 = 0;
                pport.data_set_bit6 = 0;
            }

            /* set real value of read bit 7 */
            if (pport.data_falloff_bit7 && (pport.data_set_clk_bit7 < maincpu_clk)) {
                pport.data_falloff_bit7 = 0;
                pport.data_set_bit7 = 0;
            }

            /* for unused bits in input mode, the value comes from the "capacitor" */

            /* set real value of bit 6 */
            if (!(pport.dir_read & 0x40)) {
                retval &= ~0x40;
                retval |= pport.data_set_bit6;
            }

            /* set real value of bit 7 */
            if (!(pport.dir_read & 0x80)) {
                retval &= ~0x80;
                retval |= pport.data_set_bit7;
            }

            return retval;
    }

    if (c64_256k_enabled) {
        return c64_256k_ram_segment0_read(addr);
    } else {
        if (plus256k_enabled) {
            return plus256k_ram_low_read(addr);
        } else {
            return mem_ram[addr & 0xff];
        }
    }
}

void zero_store(uint16_t addr, uint8_t value)
{
    addr &= 0xff;
#ifdef FEATURE_CPUMEMHISTORY
    monitor_memmap_store(addr, MEMMAP_RAM_W);
#endif
    switch ((uint8_t)addr) {
        case 0:
            if (vbank == 0) {
                if (c64_256k_enabled) {
                    c64_256k_ram_segment0_store((uint16_t)0, vicii_read_phi1_lowlevel());
                } else {
                    if (plus256k_enabled) {
                        plus256k_ram_low_store((uint16_t)0, vicii_read_phi1_lowlevel());
                    } else {
                        vicii_mem_vbank_store((uint16_t)0, vicii_read_phi1_lowlevel());
                    }
                }
            } else {
                mem_ram[0] = vicii_read_phi1_lowlevel();
                machine_handle_pending_alarms(maincpu_rmw_flag + 1);
            }
            /* when switching an unused bit from output (where it contained a
               stable value) to input mode (where the input is floating), some
               of the charge is transferred to the floating input */

            /* check if bit 6 has flipped */
            if ((pport.dir & 0x40)) {
                if ((pport.dir ^ value) & 0x40) {
                    pport.data_set_clk_bit6 = maincpu_clk + C64_CPU6510_DATA_PORT_FALL_OFF_CYCLES;
                    pport.data_set_bit6 = pport.data & 0x40;
                    pport.data_falloff_bit6 = 1;
                }
            }

            /* check if bit 7 has flipped */
            if ((pport.dir & 0x80)) {
                if ((pport.dir ^ value) & 0x80) {
                    pport.data_set_clk_bit7 = maincpu_clk + C64_CPU6510_DATA_PORT_FALL_OFF_CYCLES;
                    pport.data_set_bit7 = pport.data & 0x80;
                    pport.data_falloff_bit7 = 1;
                }
            }

            if (pport.dir != value) {
                pport.dir = value;
                mem_pla_config_changed();
            }
            break;
        case 1:
            if (vbank == 0) {
                if (c64_256k_enabled) {
                    c64_256k_ram_segment0_store((uint16_t)1, vicii_read_phi1_lowlevel());
                } else {
                    if (plus256k_enabled) {
                        plus256k_ram_low_store((uint16_t)1, vicii_read_phi1_lowlevel());
                    } else {
                        vicii_mem_vbank_store((uint16_t)1, vicii_read_phi1_lowlevel());
                    }
                }
            } else {
                mem_ram[1] = vicii_read_phi1_lowlevel();
                machine_handle_pending_alarms(maincpu_rmw_flag + 1);
            }

            /* when writing to an unused bit that is output, charge the "capacitor",
               otherwise don't touch it */
            if (pport.dir & 0x80) {
                pport.data_set_bit7 = value & 0x80;
                pport.data_set_clk_bit7 = maincpu_clk + C64_CPU6510_DATA_PORT_FALL_OFF_CYCLES;
                pport.data_falloff_bit7 = 1;
            }

            if (pport.dir & 0x40) {
                pport.data_set_bit6 = value & 0x40;
                pport.data_set_clk_bit6 = maincpu_clk + C64_CPU6510_DATA_PORT_FALL_OFF_CYCLES;
                pport.data_falloff_bit6 = 1;
            }

            if (pport.data != value) {
                pport.data = value;
                mem_pla_config_changed();
            }
            break;
        default:
            if (vbank == 0) {
                if (c64_256k_enabled) {
                    c64_256k_ram_segment0_store(addr, value);
                } else {
                    if (plus256k_enabled) {
                        plus256k_ram_low_store(addr, value);
                    } else {
                        vicii_mem_vbank_store(addr, value);
                    }
                }
            } else {
                mem_ram[addr] = value;
            }
    }
}

/* ------------------------------------------------------------------------- */

uint8_t chargen_read(uint16_t addr)
{
    return mem_chargen_rom[addr & 0xfff];
}

void chargen_store(uint16_t addr, uint8_t value)
{
    mem_chargen_rom[addr & 0xfff] = value;
}

uint8_t ram_read(uint16_t addr)
{
    return mem_ram[addr];
}

void ram_store(uint16_t addr, uint8_t value)
{
    mem_ram[addr] = value;
}

void ram_hi_store(uint16_t addr, uint8_t value)
{
    if (vbank == 3) {
        vicii_mem_vbank_3fxx_store(addr, value);
    } else {
        mem_ram[addr] = value;
    }

    if (addr == 0xff00) {
        reu_dma(-1);
    }
}

/* unconnected memory space */
static uint8_t void_read(uint16_t addr)
{
    return vicii_read_phi1();
}

static void void_store(uint16_t addr, uint8_t value)
{
    return;
}

/* ------------------------------------------------------------------------- */

/* Generic memory access.  */

void mem_store(uint16_t addr, uint8_t value)
{
    _mem_write_tab_ptr[addr >> 8](addr, value);
}

uint8_t mem_read(uint16_t addr)
{
    return _mem_read_tab_ptr[addr >> 8](addr);
}

void mem_store_without_ultimax(uint16_t addr, uint8_t value)
{
    store_func_ptr_t *write_tab_ptr;

    write_tab_ptr = mem_write_tab[vbank][mem_config & 7];

    write_tab_ptr[addr >> 8](addr, value);
}

uint8_t mem_read_without_ultimax(uint16_t addr)
{
    read_func_ptr_t *read_tab_ptr;

    read_tab_ptr = mem_read_tab[mem_config & 7];

    return read_tab_ptr[addr >> 8](addr);
}

void mem_store_without_romlh(uint16_t addr, uint8_t value)
{
    store_func_ptr_t *write_tab_ptr;

    write_tab_ptr = mem_write_tab[vbank][0];

    write_tab_ptr[addr >> 8](addr, value);
}

/* ------------------------------------------------------------------------- */

void colorram_store(uint16_t addr, uint8_t value)
{
    mem_color_ram[addr & 0x3ff] = value & 0xf;
}

uint8_t colorram_read(uint16_t addr)
{
    return mem_color_ram[addr & 0x3ff] | (vicii_read_phi1() & 0xf0);
}

/* ------------------------------------------------------------------------- */

/* init 256k memory table changes */
/* FIXME: make sure all cartridge related callbacks are hooked correctly */
static int check_256k_ram_write(int k, int i, int j)
{
    if (mem_write_tab[k][i][j] == vicii_mem_vbank_39xx_store) {
        return 1;
    }
    if (mem_write_tab[k][i][j] == vicii_mem_vbank_3fxx_store) {
        return 1;
    }
    if (mem_write_tab[k][i][j] == vicii_mem_vbank_store) {
        return 1;
    }
    if (mem_write_tab[k][i][j] == ram_hi_store) {
        return 1;
    }
    if (mem_write_tab[k][i][j] == ram_store) {
        return 1;
    }
    if (mem_write_tab[k][i][j] == raml_no_ultimax_store) { /* possibly breaks mmc64 and expert */
        return 1;
    }
    if (mem_write_tab[k][i][j] == romh_no_ultimax_store) { /* possibly breaks mmc64 and expert */
        return 1;
    }
    if (mem_write_tab[k][i][j] == ramh_no_ultimax_store) { /* possibly breaks mmc64 and expert */
        return 1;
    }
    if (mem_write_tab[k][i][j] == romh_store) {
        return 1;
    }
    return 0;
}

static void c64_256k_init_config(void)
{
    int i, j, k;

    if (c64_256k_enabled) {
        mem_limit_256k_init(mem_read_limit_tab);
        for (i = 0; i < NUM_CONFIGS; i++) {
            for (j = 1; j <= 0xff; j++) {
                for (k = 0; k < NUM_VBANKS; k++) {
                    if (check_256k_ram_write(k, i, j) == 1) {
                        if (j < 0x40) {
                            mem_write_tab[k][i][j] = c64_256k_ram_segment0_store;
                        }
                        if (j > 0x3f && j < 0x80) {
                            mem_write_tab[k][i][j] = c64_256k_ram_segment1_store;
                        }
                        if (j > 0x7f && j < 0xc0) {
                            mem_write_tab[k][i][j] = c64_256k_ram_segment2_store;
                        }
                        if (j > 0xbf) {
                            mem_write_tab[k][i][j] = c64_256k_ram_segment3_store;
                        }
                    }
                }
                if (mem_read_tab[i][j] == ram_read) {
                    if (j < 0x40) {
                        mem_read_tab[i][j] = c64_256k_ram_segment0_read;
                    }
                    if (j > 0x3f && j < 0x80) {
                        mem_read_tab[i][j] = c64_256k_ram_segment1_read;
                    }
                    if (j > 0x7f && j < 0xc0) {
                        mem_read_tab[i][j] = c64_256k_ram_segment2_read;
                    }
                    if (j > 0xbf) {
                        mem_read_tab[i][j] = c64_256k_ram_segment3_read;
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------- */

/* init plus256k memory table changes */
/* FIXME: make sure all cartridge related callbacks are hooked correctly */
static void plus256k_init_config(void)
{
    int i, j, k;

    if (plus256k_enabled) {
        mem_limit_256k_init(mem_read_limit_tab);
        for (i = 0; i < NUM_CONFIGS; i++) {
            for (j = 1; j <= 0xff; j++) {
                for (k = 0; k < NUM_VBANKS; k++) {
                    if (check_256k_ram_write(k, i, j) == 1) {
                        if (j < 0x10) {
                            mem_write_tab[k][i][j] = plus256k_ram_low_store;
                        } else {
                            mem_write_tab[k][i][j] = plus256k_ram_high_store;
                        }
                    }
                }
                if (mem_read_tab[i][j] == ram_read) {
                    if (j < 0x10) {
                        mem_read_tab[i][j] = plus256k_ram_low_read;
                    } else {
                        mem_read_tab[i][j] = plus256k_ram_high_read;
                    }
                }
            }
        }
    }
}

/* init plus60k memory table changes */
/* FIXME: make sure all cartridge related callbacks are hooked correctly */
static void plus60k_init_config(void)
{
    int i, j, k;

    if (plus60k_enabled) {
        mem_limit_plus60k_init(mem_read_limit_tab);
        for (i = 0; i < NUM_CONFIGS; i++) {
            for (j = 0x10; j <= 0xff; j++) {
                for (k = 0; k < NUM_VBANKS; k++) {
                    if (mem_write_tab[k][i][j] == vicii_mem_vbank_39xx_store) {
                        mem_write_tab[k][i][j] = plus60k_vicii_mem_vbank_39xx_store;
                    }
                    if (mem_write_tab[k][i][j] == vicii_mem_vbank_3fxx_store) {
                        mem_write_tab[k][i][j] = plus60k_vicii_mem_vbank_3fxx_store;
                    }
                    if (mem_write_tab[k][i][j] == vicii_mem_vbank_store) {
                        mem_write_tab[k][i][j] = plus60k_vicii_mem_vbank_store;
                    }
                    if (mem_write_tab[k][i][j] == ram_hi_store) {
                        mem_write_tab[k][i][j] = plus60k_ram_hi_store;
                    }
                    if (mem_write_tab[k][i][j] == ram_store) {
                        mem_write_tab[k][i][j] = plus60k_ram_store;
                    }
                    if (mem_write_tab[k][i][j] == raml_no_ultimax_store) {
                        mem_write_tab[k][i][j] = plus60k_ram_store; /* possibly breaks mmc64 and expert */
                    }
                    if (mem_write_tab[k][i][j] == romh_no_ultimax_store) {
                        mem_write_tab[k][i][j] = plus60k_ram_store; /* possibly breaks mmc64 and expert */
                    }
                    if (mem_write_tab[k][i][j] == ramh_no_ultimax_store) {
                        mem_write_tab[k][i][j] = plus60k_ram_store; /* possibly breaks mmc64 and expert */
                    }
                    if (mem_write_tab[k][i][j] == romh_store) {
                        mem_write_tab[k][i][j] = plus60k_ram_store;
                    }
                }
                if (mem_read_tab[i][j] == ram_read) {
                    mem_read_tab[i][j] = plus60k_ram_read;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------- */

void mem_set_write_hook(int config, int page, store_func_t *f)
{
    int i;

    for (i = 0; i < NUM_VBANKS; i++) {
        mem_write_tab[i][config][page] = f;
    }
}

void mem_read_tab_set(unsigned int base, unsigned int index, read_func_ptr_t read_func)
{
    mem_read_tab[base][index] = read_func;
}

void mem_read_base_set(unsigned int base, unsigned int index, uint8_t *mem_ptr)
{
    mem_read_base_tab[base][index] = mem_ptr;
}

void mem_initialize_memory(void)
{
    int i, j, k;
    int board;

    mem_chargen_rom_ptr = mem_chargen_rom;
    mem_color_ram_cpu = mem_color_ram;
    mem_color_ram_vicii = mem_color_ram;

    mem_limit_init(mem_read_limit_tab);

    /* setup watchpoint tables */
    mem_read_tab_watch[0] = zero_read_watch;
    mem_write_tab_watch[0] = zero_store_watch;
    for (i = 1; i <= 0x100; i++) {
        mem_read_tab_watch[i] = read_watch;
        mem_write_tab_watch[i] = store_watch;
    }

    resources_get_int("BoardType", &board);

    /* first init everything to "nothing" */
    for (i = 0; i < NUM_CONFIGS; i++) {
        for (j = 0; j <= 0xff; j++) {
            mem_read_tab[i][j] = void_read;
            mem_read_base_tab[i][j] = NULL;
            mem_set_write_hook(i, j, void_store);
        }
    }

    /* Default is RAM.  */
    for (i = 0; i < NUM_CONFIGS; i++) {
        mem_set_write_hook(i, 0, zero_store);
        mem_read_tab[i][0] = zero_read;
        mem_read_base_tab[i][0] = mem_ram;
        for (j = 1; j <= 0xfe; j++) {
            if (board == 1 && j >= 0x08) {
                /* mem_read_tab[i][j] = void_read;
                mem_read_base_tab[i][j] = NULL;
                mem_set_write_hook(0, j, void_store); */
                continue;
            }
            mem_read_tab[i][j] = ram_read;
            mem_read_base_tab[i][j] = mem_ram;
            for (k = 0; k < NUM_VBANKS; k++) {
                if ((j & 0xc0) == (k << 6)) {
                    switch (j & 0x3f) {
                        case 0x39:
                            mem_write_tab[k][i][j] = vicii_mem_vbank_39xx_store;
                            break;
                        case 0x3f:
                            mem_write_tab[k][i][j] = vicii_mem_vbank_3fxx_store;
                            break;
                        default:
                            mem_write_tab[k][i][j] = vicii_mem_vbank_store;
                    }
                } else {
                    mem_write_tab[k][i][j] = ram_store;
                }
            }
        }
        if (board == 1) {
            /* mem_read_tab[i][0xff] = void_read;
            mem_read_base_tab[i][0xff] = NULL;
            mem_set_write_hook(0, 0xff, void_store); */
        } else {
            mem_read_tab[i][0xff] = ram_read;
            mem_read_base_tab[i][0xff] = mem_ram;

            /* vbank access is handled within `ram_hi_store()'.  */
            mem_set_write_hook(i, 0xff, ram_hi_store);
        }
    }

    /* Setup character generator ROM at $D000-$DFFF (memory configs 1, 2, 3, 9, 10, 11, 26, 27).  */
    for (i = 0xd0; i <= 0xdf; i++) {
        mem_read_tab[1][i] = chargen_read;
        mem_read_tab[2][i] = chargen_read;
        mem_read_tab[3][i] = chargen_read;
        mem_read_tab[9][i] = chargen_read;
        mem_read_tab[10][i] = chargen_read;
        mem_read_tab[11][i] = chargen_read;
        mem_read_tab[26][i] = chargen_read;
        mem_read_tab[27][i] = chargen_read;
        mem_read_base_tab[1][i] = (uint8_t *)(mem_chargen_rom - (uint8_t *)0xd000);
        mem_read_base_tab[2][i] = (uint8_t *)(mem_chargen_rom - (uint8_t *)0xd000);
        mem_read_base_tab[3][i] = (uint8_t *)(mem_chargen_rom - (uint8_t *)0xd000);
        mem_read_base_tab[9][i] = (uint8_t *)(mem_chargen_rom - (uint8_t *)0xd000);
        mem_read_base_tab[10][i] = (uint8_t *)(mem_chargen_rom - (uint8_t *)0xd000);
        mem_read_base_tab[11][i] = (uint8_t *)(mem_chargen_rom - (uint8_t *)0xd000);
        mem_read_base_tab[26][i] = (uint8_t *)(mem_chargen_rom - (uint8_t *)0xd000);
        mem_read_base_tab[27][i] = (uint8_t *)(mem_chargen_rom - (uint8_t *)0xd000);
    }

    c64meminit(0);

    for (i = 0; i < NUM_CONFIGS; i++) {
        mem_read_tab[i][0x100] = mem_read_tab[i][0];
        for (j = 0; j < NUM_VBANKS; j++) {
            mem_write_tab[j][i][0x100] = mem_write_tab[j][i][0];
        }
        mem_read_base_tab[i][0x100] = mem_read_base_tab[i][0];
    }

    _mem_read_tab_ptr = mem_read_tab[7];
    _mem_write_tab_ptr = mem_write_tab[vbank][7];
    _mem_read_base_tab_ptr = mem_read_base_tab[7];
    mem_read_limit_tab_ptr = mem_read_limit_tab[7];

    vicii_set_chargen_addr_options(0x7000, 0x1000);

    c64pla_pport_reset();
    export.exrom = 0;
    export.game = 0;

    /* Setup initial memory configuration.  */
    mem_pla_config_changed();
    cartridge_init_config();
    /* internal expansions, these may modify the above mappings and must take
       care of hooking up all callbacks correctly.
    */
    plus60k_init_config();
    plus256k_init_config();
    c64_256k_init_config();

    if (board == 1) {
        mem_limit_max_init(mem_read_limit_tab);
    }
}

void mem_mmu_translate(unsigned int addr, uint8_t **base, int *start, int *limit)
{
    uint8_t *p = _mem_read_base_tab_ptr[addr >> 8];
    uint32_t limits;

    if (p != NULL && addr > 1) {
        *base = p;
        limits = mem_read_limit_tab_ptr[addr >> 8];
        *limit = limits & 0xffff;
        *start = limits >> 16;
    } else {
        cartridge_mmu_translate(addr, base, start, limit);
    }
}

/* ------------------------------------------------------------------------- */

/* Initialize RAM for power-up.  */
void mem_powerup(void)
{
    ram_init(mem_ram, 0x10000);
    cartridge_ram_init();  /* Clean cartridge ram too */
}

/* ------------------------------------------------------------------------- */

/* Change the current video bank.  Call this routine only when the vbank
   has really changed.  */
void mem_set_vbank(int new_vbank)
{
    vbank = new_vbank;

    /* Do not override watchpoints on vbank switches.  */
    if (_mem_write_tab_ptr != mem_write_tab_watch) {
        _mem_write_tab_ptr = mem_write_tab[new_vbank][mem_config];
    }

    vicii_set_vbank(new_vbank);
}

/* Set the tape sense status.  */
void mem_set_tape_sense(int sense)
{
    tape_sense = sense;
    mem_pla_config_changed();
}

/* Set the tape write in.  */
void mem_set_tape_write_in(int val)
{
    tape_write_in = val;
    mem_pla_config_changed();
}

/* Set the tape motor in.  */
void mem_set_tape_motor_in(int val)
{
    tape_motor_in = val;
    mem_pla_config_changed();
}

/* ------------------------------------------------------------------------- */

/* FIXME: this part needs to be checked.  */

void mem_get_basic_text(uint16_t *start, uint16_t *end)
{
    if (start != NULL) {
        *start = mem_ram[0x2b] | (mem_ram[0x2c] << 8);
    }
    if (end != NULL) {
        *end = mem_ram[0x2d] | (mem_ram[0x2e] << 8);
    }
}

void mem_set_basic_text(uint16_t start, uint16_t end)
{
    mem_ram[0x2b] = mem_ram[0xac] = start & 0xff;
    mem_ram[0x2c] = mem_ram[0xad] = start >> 8;
    mem_ram[0x2d] = mem_ram[0x2f] = mem_ram[0x31] = mem_ram[0xae] = end & 0xff;
    mem_ram[0x2e] = mem_ram[0x30] = mem_ram[0x32] = mem_ram[0xaf] = end >> 8;
}

/* this function should always read from the screen currently used by the kernal
   for output, normally this does just return system ram - except when the 
   videoram is not memory mapped.
   used by autostart to "read" the kernal messages
*/
uint8_t mem_read_screen(uint16_t addr)
{
    return ram_read(addr);
}

void mem_inject(uint32_t addr, uint8_t value)
{
    /* printf("mem_inject addr: %04x  value: %02x\n", addr, value); */
    if (!memory_hacks_ram_inject(addr, value)) {
        mem_ram[addr & 0xffff] = value;
    }
}

/* In banked memory architectures this will always write to the bank that
   contains the keyboard buffer and "number of keys in buffer", regardless of
   what the CPU "sees" currently.
   In all other cases this just writes to the first 64kb block, usually by
   wrapping to mem_inject().
*/
void mem_inject_key(uint16_t addr, uint8_t value)
{
    mem_inject(addr, value);
}

/* ------------------------------------------------------------------------- */

int mem_rom_trap_allowed(uint16_t addr)
{
    if (addr >= 0xe000) {
        switch (mem_config) {
            case 2:
            case 3:
            case 6:
            case 7:
            case 10:
            case 11:
            case 14:
            case 15:
            case 26:
            case 27:
            case 30:
            case 31:
                return 1;
            default:
                return 0;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------- */

/* Banked memory access functions for the monitor.  */

void store_bank_io(uint16_t addr, uint8_t byte)
{
    switch (addr & 0xff00) {
        case 0xd000:
            c64io_d000_store(addr, byte);
            break;
        case 0xd100:
            c64io_d100_store(addr, byte);
            break;
        case 0xd200:
            c64io_d200_store(addr, byte);
            break;
        case 0xd300:
            c64io_d300_store(addr, byte);
            break;
        case 0xd400:
            c64io_d400_store(addr, byte);
            break;
        case 0xd500:
            c64io_d500_store(addr, byte);
            break;
        case 0xd600:
            c64io_d600_store(addr, byte);
            break;
        case 0xd700:
            c64io_d700_store(addr, byte);
            break;
        case 0xd800:
        case 0xd900:
        case 0xda00:
        case 0xdb00:
            colorram_store(addr, byte);
            break;
        case 0xdc00:
            cia1_store(addr, byte);
            break;
        case 0xdd00:
            cia2_store(addr, byte);
            break;
        case 0xde00:
            c64io_de00_store(addr, byte);
            break;
        case 0xdf00:
            c64io_df00_store(addr, byte);
            break;
    }
    return;
}

uint8_t read_bank_io(uint16_t addr)
{
    switch (addr & 0xff00) {
        case 0xd000:
            return c64io_d000_read(addr);
        case 0xd100:
            return c64io_d100_read(addr);
        case 0xd200:
            return c64io_d200_read(addr);
        case 0xd300:
            return c64io_d300_read(addr);
        case 0xd400:
            return c64io_d400_read(addr);
        case 0xd500:
            return c64io_d500_read(addr);
        case 0xd600:
            return c64io_d600_read(addr);
        case 0xd700:
            return c64io_d700_read(addr);
        case 0xd800:
        case 0xd900:
        case 0xda00:
        case 0xdb00:
            return colorram_read(addr);
        case 0xdc00:
            return cia1_read(addr);
        case 0xdd00:
            return cia2_read(addr);
        case 0xde00:
            return c64io_de00_read(addr);
        case 0xdf00:
            return c64io_df00_read(addr);
    }
    return 0xff;
}

static uint8_t peek_bank_io(uint16_t addr)
{
    switch (addr & 0xff00) {
        case 0xd000:
            return c64io_d000_peek(addr);
        case 0xd100:
            return c64io_d100_peek(addr);
        case 0xd200:
            return c64io_d200_peek(addr);
        case 0xd300:
            return c64io_d300_peek(addr);
        case 0xd400:
            return c64io_d400_peek(addr);
        case 0xd500:
            return c64io_d500_peek(addr);
        case 0xd600:
            return c64io_d600_peek(addr);
        case 0xd700:
            return c64io_d700_peek(addr);
        case 0xd800:
        case 0xd900:
        case 0xda00:
        case 0xdb00:
            return colorram_read(addr);
        case 0xdc00:
            return cia1_peek(addr);
        case 0xdd00:
            return cia2_peek(addr);
        case 0xde00:
            return c64io_de00_peek(addr);
        case 0xdf00:
            return c64io_df00_peek(addr);
    }
    return 0xff;
}

/* ------------------------------------------------------------------------- */

/* Exported banked memory access functions for the monitor.  */

static const char *banknames[] = {
    "default",
    "cpu",
    "ram",
    "rom",
    "io",
    "cart",
    NULL
};

static const int banknums[] = { 1, 0, 1, 2, 3, 4, -1 };

const char **mem_bank_list(void)
{
    return banknames;
}

const int *mem_bank_list_nos(void) {
    return banknums;
}

int mem_bank_from_name(const char *name)
{
    int i = 0;

    while (banknames[i]) {
        if (!strcmp(name, banknames[i])) {
            return banknums[i];
        }
        i++;
    }
    return -1;
}

/* read memory with side-effects */
uint8_t mem_bank_read(int bank, uint16_t addr, void *context)
{
    switch (bank) {
        case 0:                   /* current */
            return mem_read(addr);
            break;
        case 3:                   /* io */
            if (addr >= 0xd000 && addr < 0xe000) {
                return read_bank_io(addr);
            }
            /* FALL THROUGH */
        case 4:                   /* cart */
            return cartridge_peek_mem(addr);
        case 2:                   /* rom */
            if (addr >= 0xa000 && addr <= 0xbfff) {
                return c64memrom_basic64_rom[addr & 0x1fff];
            }
            if (addr >= 0xd000 && addr <= 0xdfff) {
                return mem_chargen_rom[addr & 0x0fff];
            }
            if (addr >= 0xe000) {
                return c64memrom_kernal64_rom[addr & 0x1fff];
            }
            /* FALL THROUGH */
        case 1:                   /* ram */
            break;
    }
    return mem_ram[addr];
}

/* used by monitor if sfx off, and when disassembling/tracing. this function
 * can NOT use the generic mem_read stuff, because that DOES have side effects,
 * such as (re)triggering checkpoints in the monitor!
 */
uint8_t mem_bank_peek(int bank, uint16_t addr, void *context)
{
    switch (bank) {
        case 0:                   /* current */
            /* we must check for which bank is currently active */
            if (c64meminit_io_config[mem_config]) {
                if ((addr >= 0xd000) && (addr < 0xe000)) {
                    return peek_bank_io(addr);
                }
            }
            if (c64meminit_roml_config[mem_config]) {
                if (addr >= 0x8000 && addr <= 0x9fff) {
                    return cartridge_peek_mem(addr);
                }
            }
            if (c64meminit_romh_config[mem_config]) {
                unsigned int romhloc = c64meminit_romh_mapping[mem_config] << 8;
                if (addr >= romhloc && addr <= (romhloc + 0x1fff)) {
                    return cartridge_peek_mem(addr);
                }
            }
            if (c64meminit_io_config[mem_config] == 2) {
                /* ultimax mode */
                if (addr >= 0x0000 && addr <= 0x0fff) {
                    return mem_ram[addr];
                }
                return cartridge_peek_mem(addr);
            }
            if((mem_config == 3) || (mem_config == 7) ||
               (mem_config == 11) || (mem_config == 15)) {
                if (addr >= 0xa000 && addr <= 0xbfff) {
                    return c64memrom_basic64_rom[addr & 0x1fff];
                }
            }
            if((mem_config & 3) > 1) {
                if (addr >= 0xe000) {
                    return c64memrom_kernal64_rom[addr & 0x1fff];
                }
            }
            if((mem_config & 3) && (mem_config != 0x19)) {
                if ((addr >= 0xd000) && (addr < 0xdfff)) {
                    return mem_chargen_rom[addr & 0x0fff];
                }
            }
            break;
        case 3:                   /* io */
            if (addr >= 0xd000 && addr < 0xe000) {
                return peek_bank_io(addr);
            }
            /* FALL THROUGH */
        case 4:                   /* cart */
            return cartridge_peek_mem(addr);
        case 2:                   /* rom */
            if (addr >= 0xa000 && addr <= 0xbfff) {
                return c64memrom_basic64_rom[addr & 0x1fff];
            }
            if (addr >= 0xd000 && addr <= 0xdfff) {
                return mem_chargen_rom[addr & 0x0fff];
            }
            if (addr >= 0xe000) {
                return c64memrom_kernal64_rom[addr & 0x1fff];
            }
            /* FALL THROUGH */
        case 1:                   /* ram */
            break;
    }
    return mem_ram[addr];
}

void mem_bank_write(int bank, uint16_t addr, uint8_t byte, void *context)
{
    switch (bank) {
        case 0:                   /* current */
            mem_store(addr, byte);
            return;
        case 3:                   /* io */
            if (addr >= 0xd000 && addr < 0xe000) {
                store_bank_io(addr, byte);
                return;
            }
            /* FALL THROUGH */
        case 2:                   /* rom */
            if (addr >= 0xa000 && addr <= 0xbfff) {
                return;
            }
            if (addr >= 0xd000 && addr <= 0xdfff) {
                return;
            }
            if (addr >= 0xe000) {
                return;
            }
            /* FALL THROUGH */
        case 1:                   /* ram */
            break;
    }
    mem_ram[addr] = byte;
}

/* used by monitor if sfx off */
void mem_bank_poke(int bank, uint16_t addr, uint8_t byte, void *context)
{
    mem_bank_write(bank, addr, byte, context);
}

static int mem_dump_io(void *context, uint16_t addr)
{
    if ((addr >= 0xdc00) && (addr <= 0xdc3f)) {
        return ciacore_dump(machine_context.cia1);
    } else if ((addr >= 0xdd00) && (addr <= 0xdd3f)) {
        return ciacore_dump(machine_context.cia2);
    }
    return -1;
}

mem_ioreg_list_t *mem_ioreg_list_get(void *context)
{
    mem_ioreg_list_t *mem_ioreg_list = NULL;

    mon_ioreg_add_list(&mem_ioreg_list, "CIA1", 0xdc00, 0xdc0f, mem_dump_io, NULL);
    mon_ioreg_add_list(&mem_ioreg_list, "CIA2", 0xdd00, 0xdd0f, mem_dump_io, NULL);

    io_source_ioreg_add_list(&mem_ioreg_list);

    return mem_ioreg_list;
}

void mem_get_screen_parameter(uint16_t *base, uint8_t *rows, uint8_t *columns, int *bank)
{
    *base = ((vicii_peek(0xd018) & 0xf0) << 6) | ((~cia2_peek(0xdd00) & 0x03) << 14);
    *rows = 25;
    *columns = 40;
    *bank = 0;
}

/* used by autostart to locate and "read" kernal output on the current screen
 * this function should return whatever the kernal currently uses, regardless
 * what is currently visible/active in the UI 
 */
void mem_get_cursor_parameter(uint16_t *screen_addr, uint8_t *cursor_column, uint8_t *line_length, int *blinking)
{
    /* Cursor Blink enable: 1 = Flash Cursor, 0 = Cursor disabled, -1 = n/a */
    *blinking = mem_ram[0xcc] ? 0 : 1;
    *screen_addr = mem_ram[0xd1] + mem_ram[0xd2] * 256; /* Current Screen Line Address */
    *cursor_column = mem_ram[0xd3];    /* Cursor Column on Current Line */
    *line_length = mem_ram[0xd5] + 1;  /* Physical Screen Line Length */
}

/* ------------------------------------------------------------------------- */

void mem_color_ram_to_snapshot(uint8_t *color_ram)
{
    memcpy(color_ram, mem_color_ram, 0x400);
}

void mem_color_ram_from_snapshot(uint8_t *color_ram)
{
    memcpy(mem_color_ram, color_ram, 0x400);
}

/* ------------------------------------------------------------------------- */

/* UI functions (used to distinguish between x64 and x64sc) */
int c64_mem_ui_init_early(void)
{
    return c64ui_init_early();
}

int c64_mem_ui_init(void)
{
    return c64ui_init();
}

void c64_mem_ui_shutdown(void)
{
    c64ui_shutdown();
}
