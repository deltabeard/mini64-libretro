/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - savestates.c                                            *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2012 CasualJames                                        *
 *   Copyright (C) 2009 Olejl Tillin9                                      *
 *   Copyright (C) 2008 Richard42 Tillin9                                  *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* strdup */
#define _POSIX_C_SOURCE 200809L

#ifdef USE_SDL
#include <SDL.h>
#include <SDL_thread.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <zlib.h>

#define M64P_CORE_PROTOTYPES 1
#include "api/callbacks.h"
#include "api/config.h"
#include "api/m64p_config.h"
#include "api/m64p_types.h"
#include "backends/api/storage_backend.h"
#include "device/device.h"
#include "main/list.h"
#include "main/main.h"
#include "osal/preproc.h"
#include "plugin/plugin.h"
#include "rom.h"
#include "savestates.h"
#include "util.h"
#include "workqueue.h"

enum { GB_CART_FINGERPRINT_SIZE = 0x1c };
enum { GB_CART_FINGERPRINT_OFFSET = 0x134 };

enum { DD_DISK_ID_OFFSET = 0x43670 };

static const char* savestate_magic = "M64+SAVE";
static const int savestate_latest_version = 0x00010500;  /* 1.5 */

static savestates_job job = savestates_job_nothing;
static savestates_type type = savestates_type_unknown;
static char *fname = NULL;

static unsigned int slot = 0;
static int autoinc_save_slot = 0;

struct savestate_work {
    char *filepath;
    char *data;
    size_t size;
    struct work_struct work;
#ifdef __LIBRETRO__
    void *mempointer;
#endif
};

/* Returns the malloc'd full path of the currently selected savestate. */
static char *savestates_generate_path(savestates_type type)
{
    if(fname != NULL) /* A specific path was given. */
    {
        return strdup(fname);
    }
    else /* Use the selected savestate slot */
    {
        char *filename;
        switch (type)
        {
            case savestates_type_m64p:
                filename = formatstr("%s.st%d", ROM_SETTINGS.goodname, slot);
                break;
            default:
                filename = NULL;
                break;
        }

        if (filename != NULL)
        {
            char *filepath = formatstr("%s%s", get_savestatepath(), filename);
            free(filename);
            return filepath;
        }
        else
            return NULL;
    }
}

void savestates_select_slot(unsigned int s)
{
    if(s>9||s==slot)
        return;
    slot = s;
    ConfigSetParameter(g_CoreConfig, "CurrentStateSlot", M64TYPE_INT, &s);
    StateChanged(M64CORE_SAVESTATE_SLOT, slot);

    main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Selected state slot: %d", slot);
}

/* Returns the currently selected save slot. */
unsigned int savestates_get_slot(void)
{
    return slot;
}

/* Sets save state slot autoincrement on or off. */
void savestates_set_autoinc_slot(int b)
{
    autoinc_save_slot = b;
}

void savestates_inc_slot(void)
{
    if(++slot>9)
        slot = 0;
    StateChanged(M64CORE_SAVESTATE_SLOT, slot);
}

savestates_job savestates_get_job(void)
{
    return job;
}

void savestates_set_job(savestates_job j, savestates_type t, const char *fn)
{
    if (fname != NULL)
    {
        free(fname);
        fname = NULL;
    }

    job = j;
    type = t;
    if (fn != NULL)
        fname = strdup(fn);
}

static void savestates_clear_job(void)
{
    savestates_set_job(savestates_job_nothing, savestates_type_unknown, NULL);
}

#define GETARRAY(buff, type, count) \
    (to_little_endian_buffer(buff, sizeof(type),count), \
     buff += count*sizeof(type), \
     (type *)(buff-count*sizeof(type)))
#define COPYARRAY(dst, buff, type, count) \
    memcpy(dst, GETARRAY(buff, type, count), sizeof(type)*count)
#define GETDATA(buff, type) *GETARRAY(buff, type, 1)
#define PUTARRAY(src, buff, type, count) \
    memcpy(buff, src, sizeof(type)*count); \
    to_little_endian_buffer(buff, sizeof(type), count); \
    buff += count*sizeof(type);

#define PUTDATA(buff, type, value) \
    do { type x = value; PUTARRAY(&x, buff, type, 1); } while(0)

#ifndef __LIBRETRO__
int savestates_load_m64p(struct device* dev, char *filepath)
#else
int savestates_load_m64p(struct device* dev, const void *data)
#endif
{
    unsigned char header[44];
    unsigned int version;
    int i;
    uint32_t FCR31;

    size_t savestateSize;
    unsigned char *savestateData, *curr;
    char queue[1024];
    unsigned char using_tlb_data[4];
    unsigned char data_0001_0200[4096]; // 4k for extra state from v1.2
    uint64_t flashram_status;

    uint32_t* cp0_regs = r4300_cp0_regs(&dev->r4300.cp0);

#ifdef USE_SDL
    SDL_LockMutex(savestates_lock);
#endif

#ifndef __LIBRETRO__
    gzFile f;
    f = gzopen(filepath, "rb");
    if(f==NULL)
    {
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Could not open state file: %s", filepath);
#ifdef USE_SDL
        SDL_UnlockMutex(savestates_lock);
#endif
        return 0;
    }

    /* Read and check Mupen64Plus magic number. */
    if (gzread(f, header, 44) != 44)
    {
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Could not read header from state file %s", filepath);
        gzclose(f);
#ifdef USE_SDL
        SDL_UnlockMutex(savestates_lock);
#endif
        return 0;
    }
    curr = header;

    if(strncmp((char *)curr, savestate_magic, 8)!=0)
    {
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "State file: %s is not a valid Mupen64plus savestate.", filepath);
        gzclose(f);
#ifdef USE_SDL
        SDL_UnlockMutex(savestates_lock);
#endif
        return 0;
    }
#else
    memcpy(header, data, 44);
    curr = header;
    if(strncmp((char *)curr, savestate_magic, 8)!=0)
    {
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Savestate is not a valid Mupen64plus savestate.");
        return 0;
    }
#endif

    curr += 8;

    version = *curr++;
    version = (version << 8) | *curr++;
    version = (version << 8) | *curr++;
    version = (version << 8) | *curr++;
    if((version >> 16) != (savestate_latest_version >> 16))
    {
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "State version (%08x) isn't compatible. Please update Mupen64Plus.", version);
#ifndef __LIBRETRO__
        gzclose(f);
#endif
#ifdef USE_SDL
        SDL_UnlockMutex(savestates_lock);
#endif
        return 0;
    }

    if(memcmp((char *)curr, ROM_SETTINGS.MD5, 32))
    {
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "State ROM MD5 does not match current ROM.");
#ifndef __LIBRETRO__
        gzclose(f);
#endif
#ifdef USE_SDL
        SDL_UnlockMutex(savestates_lock);
#endif
        return 0;
    }
    curr += 32;

    /* Read the rest of the savestate */
    savestateSize = 16788244;
    savestateData = curr = (unsigned char *)malloc(savestateSize);
    if (savestateData == NULL)
    {
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Insufficient memory to load state.");
#ifndef __LIBRETRO__
        gzclose(f);
#endif
#ifdef USE_SDL
        SDL_UnlockMutex(savestates_lock);
#endif
        return 0;
    }
    if (version == 0x00010000) /* original savestate version */
    {
#ifndef __LIBRETRO__
        if (gzread(f, savestateData, savestateSize) != savestateSize ||
            (gzread(f, queue, sizeof(queue)) % 4) != 0)
        {
            main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Could not read Mupen64Plus savestate 1.0 data from %s", filepath);
            free(savestateData);
            gzclose(f);
#ifdef USE_SDL
            SDL_UnlockMutex(savestates_lock);
#endif
            return 0;
        }
#else
        memcpy(savestateData, data + 44, savestateSize);
        memcpy(queue, data + 44 + savestateSize, sizeof(queue));
#endif
    }
    else if (version == 0x00010100) // saves entire eventqueue plus 4-byte using_tlb flags
    {
#ifndef __LIBRETRO__
        if (gzread(f, savestateData, savestateSize) != savestateSize ||
            gzread(f, queue, sizeof(queue)) != sizeof(queue) ||
            gzread(f, using_tlb_data, sizeof(using_tlb_data)) != sizeof(using_tlb_data))
        {
            main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Could not read Mupen64Plus savestate 1.1 data from %s", filepath);
            free(savestateData);
            gzclose(f);
#ifdef USE_SDL
            SDL_UnlockMutex(savestates_lock);
#endif
            return 0;
        }
#else
        memcpy(savestateData, data + 44, savestateSize);
        memcpy(queue, data + 44 + savestateSize, sizeof(queue));
        memcpy(using_tlb_data, data + 44 + savestateSize + sizeof(queue), sizeof(using_tlb_data));
#endif
    }
    else // version >= 0x00010200  saves entire eventqueue, 4-byte using_tlb flags and extra state
    {
#ifndef __LIBRETRO__
        if (gzread(f, savestateData, savestateSize) != (int)savestateSize ||
            gzread(f, queue, sizeof(queue)) != sizeof(queue) ||
            gzread(f, using_tlb_data, sizeof(using_tlb_data)) != sizeof(using_tlb_data) ||
            gzread(f, data_0001_0200, sizeof(data_0001_0200)) != sizeof(data_0001_0200))
        {
            main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Could not read Mupen64Plus savestate 1.2+ data from %s", filepath);
            free(savestateData);
            gzclose(f);
#ifdef USE_SDL
            SDL_UnlockMutex(savestates_lock);
#endif
            return 0;
        }
#else
        memcpy(savestateData, data + 44, savestateSize);
        memcpy(queue, data + 44 + savestateSize, sizeof(queue));
        memcpy(using_tlb_data, data + 44 + savestateSize + sizeof(queue), sizeof(using_tlb_data));
        memcpy(data_0001_0200, data + 44 + savestateSize + sizeof(queue) + sizeof(using_tlb_data), sizeof(data_0001_0200));
#endif
    }

#ifndef __LIBRETRO__
    gzclose(f);
#endif
#ifdef USE_SDL
    SDL_UnlockMutex(savestates_lock);
#endif

    // Parse savestate
    dev->rdram.regs[0][RDRAM_CONFIG_REG]       = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_DEVICE_ID_REG]    = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_DELAY_REG]        = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_MODE_REG]         = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_REF_INTERVAL_REG] = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_REF_ROW_REG]      = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_RAS_INTERVAL_REG] = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_MIN_INTERVAL_REG] = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_ADDR_SELECT_REG]  = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_DEVICE_MANUF_REG] = GETDATA(curr, uint32_t);

    curr += 4; /* Padding from old implementation */
    dev->mi.regs[MI_INIT_MODE_REG] = GETDATA(curr, uint32_t);
    curr += 4; // Duplicate MI init mode flags from old implementation
    dev->mi.regs[MI_VERSION_REG]   = GETDATA(curr, uint32_t);
    dev->mi.regs[MI_INTR_REG]      = GETDATA(curr, uint32_t);
    dev->mi.regs[MI_INTR_MASK_REG] = GETDATA(curr, uint32_t);
    curr += 4; /* Padding from old implementation */
    curr += 8; // Duplicated MI intr flags and padding from old implementation

    dev->pi.regs[PI_DRAM_ADDR_REG]    = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_CART_ADDR_REG]    = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_RD_LEN_REG]       = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_WR_LEN_REG]       = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_STATUS_REG]       = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM1_LAT_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM1_PWD_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM1_PGS_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM1_RLS_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM2_LAT_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM2_PWD_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM2_PGS_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM2_RLS_REG] = GETDATA(curr, uint32_t);

    dev->sp.regs[SP_MEM_ADDR_REG]  = GETDATA(curr, uint32_t);
    dev->sp.regs[SP_DRAM_ADDR_REG] = GETDATA(curr, uint32_t);
    dev->sp.regs[SP_RD_LEN_REG]    = GETDATA(curr, uint32_t);
    dev->sp.regs[SP_WR_LEN_REG]    = GETDATA(curr, uint32_t);
    curr += 4; /* Padding from old implementation */
    dev->sp.regs[SP_STATUS_REG]    = GETDATA(curr, uint32_t);
    curr += 16; // Duplicated SP flags and padding from old implementation
    dev->sp.regs[SP_DMA_FULL_REG]  = GETDATA(curr, uint32_t);
    dev->sp.regs[SP_DMA_BUSY_REG]  = GETDATA(curr, uint32_t);
    dev->sp.regs[SP_SEMAPHORE_REG] = GETDATA(curr, uint32_t);

    dev->sp.regs2[SP_PC_REG]    = GETDATA(curr, uint32_t);
    dev->sp.regs2[SP_IBIST_REG] = GETDATA(curr, uint32_t);

    dev->si.regs[SI_DRAM_ADDR_REG]      = GETDATA(curr, uint32_t);
    dev->si.regs[SI_PIF_ADDR_RD64B_REG] = GETDATA(curr, uint32_t);
    dev->si.regs[SI_PIF_ADDR_WR64B_REG] = GETDATA(curr, uint32_t);
    dev->si.regs[SI_STATUS_REG]         = GETDATA(curr, uint32_t);

    dev->vi.regs[VI_STATUS_REG]  = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_ORIGIN_REG]  = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_WIDTH_REG]   = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_V_INTR_REG]  = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_CURRENT_REG] = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_BURST_REG]   = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_V_SYNC_REG]  = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_H_SYNC_REG]  = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_LEAP_REG]    = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_H_START_REG] = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_V_START_REG] = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_V_BURST_REG] = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_X_SCALE_REG] = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_Y_SCALE_REG] = GETDATA(curr, uint32_t);
    dev->vi.delay = GETDATA(curr, uint32_t);
    gfx.viStatusChanged();
    gfx.viWidthChanged();

    dev->ri.regs[RI_MODE_REG]         = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_CONFIG_REG]       = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_CURRENT_LOAD_REG] = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_SELECT_REG]       = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_REFRESH_REG]      = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_LATENCY_REG]      = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_ERROR_REG]        = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_WERROR_REG]       = GETDATA(curr, uint32_t);

    dev->ai.regs[AI_DRAM_ADDR_REG] = GETDATA(curr, uint32_t);
    dev->ai.regs[AI_LEN_REG]       = GETDATA(curr, uint32_t);
    dev->ai.regs[AI_CONTROL_REG]   = GETDATA(curr, uint32_t);
    dev->ai.regs[AI_STATUS_REG]    = GETDATA(curr, uint32_t);
    dev->ai.regs[AI_DACRATE_REG]   = GETDATA(curr, uint32_t);
    dev->ai.regs[AI_BITRATE_REG]   = GETDATA(curr, uint32_t);
    dev->ai.fifo[1].duration  = GETDATA(curr, uint32_t);
    dev->ai.fifo[1].length = GETDATA(curr, uint32_t);
    dev->ai.fifo[0].duration  = GETDATA(curr, uint32_t);
    dev->ai.fifo[0].length = GETDATA(curr, uint32_t);
    /* best effort initialization of fifo addresses...
     * You might get a small sound "pop" because address might be wrong.
     * Proper initialization requires changes to savestate format
     */
    dev->ai.fifo[0].address = dev->ai.regs[AI_DRAM_ADDR_REG];
    dev->ai.fifo[1].address = dev->ai.regs[AI_DRAM_ADDR_REG];
    dev->ai.samples_format_changed = 1;

    dev->dp.dpc_regs[DPC_START_REG]    = GETDATA(curr, uint32_t);
    dev->dp.dpc_regs[DPC_END_REG]      = GETDATA(curr, uint32_t);
    dev->dp.dpc_regs[DPC_CURRENT_REG]  = GETDATA(curr, uint32_t);
    curr += 4; // Padding from old implementation
    dev->dp.dpc_regs[DPC_STATUS_REG]   = GETDATA(curr, uint32_t);
    curr += 12; // Duplicated DPC flags and padding from old implementation
    dev->dp.dpc_regs[DPC_CLOCK_REG]    = GETDATA(curr, uint32_t);
    dev->dp.dpc_regs[DPC_BUFBUSY_REG]  = GETDATA(curr, uint32_t);
    dev->dp.dpc_regs[DPC_PIPEBUSY_REG] = GETDATA(curr, uint32_t);
    dev->dp.dpc_regs[DPC_TMEM_REG]     = GETDATA(curr, uint32_t);

    dev->dp.dps_regs[DPS_TBIST_REG]        = GETDATA(curr, uint32_t);
    dev->dp.dps_regs[DPS_TEST_MODE_REG]    = GETDATA(curr, uint32_t);
    dev->dp.dps_regs[DPS_BUFTEST_ADDR_REG] = GETDATA(curr, uint32_t);
    dev->dp.dps_regs[DPS_BUFTEST_DATA_REG] = GETDATA(curr, uint32_t);

    COPYARRAY(dev->rdram.dram, curr, uint32_t, RDRAM_MAX_SIZE/4);
    COPYARRAY(dev->sp.mem, curr, uint32_t, SP_MEM_SIZE/4);
    COPYARRAY(dev->pif.ram, curr, uint8_t, PIF_RAM_SIZE);

    dev->cart.use_flashram = GETDATA(curr, int32_t);
    dev->cart.flashram.mode = GETDATA(curr, int32_t);
    flashram_status = GETDATA(curr, uint64_t);
    dev->cart.flashram.status[0] = (uint32_t)(flashram_status >> 32);
    dev->cart.flashram.status[1] = (uint32_t)(flashram_status);
    dev->cart.flashram.erase_offset = GETDATA(curr, uint32_t);
    dev->cart.flashram.write_pointer = GETDATA(curr, uint32_t);

    COPYARRAY(dev->r4300.cp0.tlb.LUT_r, curr, uint32_t, 0x100000);
    COPYARRAY(dev->r4300.cp0.tlb.LUT_w, curr, uint32_t, 0x100000);

    *r4300_llbit(&dev->r4300) = GETDATA(curr, uint32_t);
    COPYARRAY(r4300_regs(&dev->r4300), curr, int64_t, 32);
    COPYARRAY(cp0_regs, curr, uint32_t, CP0_REGS_COUNT);
    *r4300_mult_lo(&dev->r4300) = GETDATA(curr, int64_t);
    *r4300_mult_hi(&dev->r4300) = GETDATA(curr, int64_t);
    cp1_reg *cp1_regs = r4300_cp1_regs(&dev->r4300.cp1);
    COPYARRAY(&cp1_regs->dword, curr, int64_t, 32);
    *r4300_cp1_fcr0(&dev->r4300.cp1)  = GETDATA(curr, uint32_t);
    FCR31 = GETDATA(curr, uint32_t);
    *r4300_cp1_fcr31(&dev->r4300.cp1) = FCR31;
    set_fpr_pointers(&dev->r4300.cp1, cp0_regs[CP0_STATUS_REG]);
    update_x86_rounding_mode(&dev->r4300.cp1);

    for (i = 0; i < 32; i++)
    {
        dev->r4300.cp0.tlb.entries[i].mask = GETDATA(curr, int16_t);
        curr += 2;
        dev->r4300.cp0.tlb.entries[i].vpn2 = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].g = GETDATA(curr, char);
        dev->r4300.cp0.tlb.entries[i].asid = GETDATA(curr, unsigned char);
        curr += 2;
        dev->r4300.cp0.tlb.entries[i].pfn_even = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].c_even = GETDATA(curr, char);
        dev->r4300.cp0.tlb.entries[i].d_even = GETDATA(curr, char);
        dev->r4300.cp0.tlb.entries[i].v_even = GETDATA(curr, char);
        curr++;
        dev->r4300.cp0.tlb.entries[i].pfn_odd = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].c_odd = GETDATA(curr, char);
        dev->r4300.cp0.tlb.entries[i].d_odd = GETDATA(curr, char);
        dev->r4300.cp0.tlb.entries[i].v_odd = GETDATA(curr, char);
        dev->r4300.cp0.tlb.entries[i].r = GETDATA(curr, char);

        dev->r4300.cp0.tlb.entries[i].start_even = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].end_even = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].phys_even = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].start_odd = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].end_odd = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].phys_odd = GETDATA(curr, uint32_t);
    }

    savestates_load_set_pc(&dev->r4300, GETDATA(curr, uint32_t));

    *r4300_cp0_next_interrupt(&dev->r4300.cp0) = GETDATA(curr, uint32_t);
    dev->vi.next_vi = GETDATA(curr, uint32_t);
    dev->vi.field = GETDATA(curr, uint32_t);

    // assert(savestateData+savestateSize == curr)

    to_little_endian_buffer(queue, 4, 256);
    load_eventqueue_infos(&dev->r4300.cp0, queue);

#ifdef NEW_DYNAREC
    // Default this to 1, it will be updated by the correct save state version later
    stop_after_jal = 1;

    if (version >= 0x00010100)
    {
        curr = using_tlb_data;
        using_tlb = GETDATA(curr, uint32_t);
    }
#endif

    if (version == 0x00010200)
    {
        union
        {
            uint8_t bytes[8];
            uint64_t force_alignment;
        } aligned;

#define ALIGNED_GETDATA(buff, type) \
    (COPYARRAY(aligned.bytes, buff, uint8_t, sizeof(type)), *(type*)aligned.bytes)

        curr = data_0001_0200;

        /* extra ai state */
        dev->ai.last_read = GETDATA(curr, uint32_t);
        dev->ai.delayed_carry = GETDATA(curr, uint32_t);

        /* extra cart_rom state */
        dev->cart.cart_rom.last_write = GETDATA(curr, uint32_t);
        dev->cart.cart_rom.rom_written = GETDATA(curr, uint32_t);

        /* extra sp state */
        curr += 4; /* here there used to be rsp_task_locked */

        /* extra af-rtc state */
        dev->cart.af_rtc.control = GETDATA(curr, uint16_t);
        dev->cart.af_rtc.now = (time_t)ALIGNED_GETDATA(curr, int64_t);
        dev->cart.af_rtc.last_update_rtc = (time_t)ALIGNED_GETDATA(curr, int64_t);

        /* extra controllers state */
        for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {
            dev->controllers[i].status = GETDATA(curr, uint8_t);
        }

        /* extra rpak state */
        for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {
            uint8_t rpk_state = GETDATA(curr, uint8_t);

            /* init rumble pak state if enabled and not controlled by the input plugin */
            if (ROM_SETTINGS.rumble && !Controls[i].RawData) {
                set_rumble_reg(&dev->rumblepaks[i], rpk_state);
            }
        }

        /* extra tpak state */
        for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {
            char gb_fingerprint[GB_CART_FINGERPRINT_SIZE];
            uint8_t rtc_regs[MBC3_RTC_REGS_COUNT];
            uint8_t rtc_latched_regs[MBC3_RTC_REGS_COUNT];
            uint8_t cam_regs[POCKET_CAM_REGS_COUNT];
            unsigned int rom_bank, ram_bank, ram_enable, mbc1_mode, rtc_latch;
            time_t rtc_last_time;

            unsigned int enabled = ALIGNED_GETDATA(curr, uint32_t);
            unsigned int bank = ALIGNED_GETDATA(curr, uint32_t);
            unsigned int access_mode = ALIGNED_GETDATA(curr, uint32_t);
            unsigned int access_mode_changed = ALIGNED_GETDATA(curr, uint32_t);
            COPYARRAY(gb_fingerprint, curr, uint8_t, GB_CART_FINGERPRINT_SIZE);
            if (gb_fingerprint[0] != 0) {
                rom_bank = ALIGNED_GETDATA(curr, uint32_t);
                ram_bank = ALIGNED_GETDATA(curr, uint32_t);
                ram_enable = ALIGNED_GETDATA(curr, uint32_t);
                mbc1_mode = ALIGNED_GETDATA(curr, uint32_t);
                COPYARRAY(rtc_regs, curr, uint8_t, MBC3_RTC_REGS_COUNT);
                rtc_latch = ALIGNED_GETDATA(curr, uint32_t);
                COPYARRAY(rtc_latched_regs, curr, uint8_t, MBC3_RTC_REGS_COUNT);
                rtc_last_time = (time_t)ALIGNED_GETDATA(curr, int64_t);
                COPYARRAY(cam_regs, curr, uint8_t, POCKET_CAM_REGS_COUNT);
            }

            if (ROM_SETTINGS.transferpak && !Controls[i].RawData) {

                /* init transferpak state if enabled and not controlled by input plugin */
                dev->transferpaks[i].enabled = enabled;
                dev->transferpaks[i].bank = bank;
                dev->transferpaks[i].access_mode = access_mode;
                dev->transferpaks[i].access_mode_changed = access_mode_changed;

                /* if it holds a valid cartridge init gbcart */
                if (dev->transferpaks[i].gb_cart != NULL
                 && dev->transferpaks[i].gb_cart->irom_storage != NULL) {
                    const uint8_t* rom = dev->transferpaks[i].gb_cart->irom_storage->data
                                        (dev->transferpaks[i].gb_cart->rom_storage);

                    /* verify that gb cart saved in savestate is the same
                     * as what is currently inserted in transferpak */
                    if (gb_fingerprint[0] != 0
                     && memcmp(gb_fingerprint,
                               rom + GB_CART_FINGERPRINT_OFFSET,
                               GB_CART_FINGERPRINT_SIZE) == 0) {

                        /* init gbcart state */
                        dev->transferpaks[i].gb_cart->rom_bank = rom_bank;
                        dev->transferpaks[i].gb_cart->ram_bank = ram_bank;
                        dev->transferpaks[i].gb_cart->ram_enable = ram_enable;
                        dev->transferpaks[i].gb_cart->mbc1_mode = mbc1_mode;
                        dev->transferpaks[i].gb_cart->rtc.latch = rtc_latch;
                        dev->transferpaks[i].gb_cart->rtc.last_time = rtc_last_time;

                        memcpy(dev->transferpaks[i].gb_cart->rtc.regs, rtc_regs, MBC3_RTC_REGS_COUNT);
                        memcpy(dev->transferpaks[i].gb_cart->rtc.latched_regs, rtc_latched_regs, MBC3_RTC_REGS_COUNT);
                        memcpy(dev->transferpaks[i].gb_cart->cam.regs, cam_regs, POCKET_CAM_REGS_COUNT);
                    }
                    else {
                        DebugMessage(M64MSG_WARNING,
                            "Savestate GB cart mismatch. Current GB cart: %s. Expected GB cart : %s",
                            (rom[GB_CART_FINGERPRINT_OFFSET] == 0x00) ? "(none)" : (const char*)(rom + GB_CART_FINGERPRINT_OFFSET),
                            (gb_fingerprint[0] == 0x00) ? "(none)" : gb_fingerprint);

                        poweron_gb_cart(dev->transferpaks[i].gb_cart);
                    }
                }
            }
        }

        /* extra pif channels state */
        for (i = 0; i < PIF_CHANNELS_COUNT; ++i) {
            int offset = GETDATA(curr, int8_t);
            if (offset >= 0) {
                setup_pif_channel(&dev->pif.channels[i], dev->pif.ram + offset);
            }
            else {
                disable_pif_channel(&dev->pif.channels[i]);
            }
        }

        /* extra vi state */
        dev->vi.count_per_scanline = ALIGNED_GETDATA(curr, uint32_t);

        /* extra si state */
        dev->si.dma_dir = GETDATA(curr, uint8_t);

        /* extra dp state */
        dev->dp.do_on_unfreeze = GETDATA(curr, uint8_t);

        /* extra RDRAM register state */
        for (i = 1; i < RDRAM_MAX_MODULES_COUNT; ++i) {
            dev->rdram.regs[i][RDRAM_CONFIG_REG]       = ALIGNED_GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_DEVICE_ID_REG]    = ALIGNED_GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_DELAY_REG]        = ALIGNED_GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_MODE_REG]         = ALIGNED_GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_REF_INTERVAL_REG] = ALIGNED_GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_REF_ROW_REG]      = ALIGNED_GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_RAS_INTERVAL_REG] = ALIGNED_GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_MIN_INTERVAL_REG] = ALIGNED_GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_ADDR_SELECT_REG]  = ALIGNED_GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_DEVICE_MANUF_REG] = ALIGNED_GETDATA(curr, uint32_t);
        }
    }
    else if (version >= 0x00010300)
    {
        curr = data_0001_0200;

        /* extra ai state */
        dev->ai.last_read = GETDATA(curr, uint32_t);
        dev->ai.delayed_carry = GETDATA(curr, uint32_t);

        /* extra cart_rom state */
        dev->cart.cart_rom.last_write = GETDATA(curr, uint32_t);
        dev->cart.cart_rom.rom_written = GETDATA(curr, uint32_t);

        /* extra sp state */
        curr += 4; /* here there used to be rsp_task_locked */

        /* extra af-rtc state */
        dev->cart.af_rtc.control = GETDATA(curr, uint16_t);
        curr += 2; /* padding to keep things 8-byte aligned */
        dev->cart.af_rtc.now = (time_t)GETDATA(curr, int64_t);
        dev->cart.af_rtc.last_update_rtc = (time_t)GETDATA(curr, int64_t);

        /* extra controllers state */
        for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {
            dev->controllers[i].status = GETDATA(curr, uint8_t);
        }

        /* extra rpak state */
        for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {
            uint8_t rpk_state = GETDATA(curr, uint8_t);

            /* init rumble pak state if enabled and not controlled by the input plugin */
            if (ROM_SETTINGS.rumble && !Controls[i].RawData) {
                set_rumble_reg(&dev->rumblepaks[i], rpk_state);
            }
        }

        /* extra tpak state */
        for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {
            char gb_fingerprint[GB_CART_FINGERPRINT_SIZE];
            uint8_t rtc_regs[MBC3_RTC_REGS_COUNT];
            uint8_t rtc_latched_regs[MBC3_RTC_REGS_COUNT];
            uint8_t cam_regs[POCKET_CAM_REGS_COUNT];
            unsigned int rom_bank, ram_bank, ram_enable, mbc1_mode, rtc_latch;
            time_t rtc_last_time;

            unsigned int enabled = GETDATA(curr, uint32_t);
            unsigned int bank = GETDATA(curr, uint32_t);
            unsigned int access_mode = GETDATA(curr, uint32_t);
            unsigned int access_mode_changed = GETDATA(curr, uint32_t);
            COPYARRAY(gb_fingerprint, curr, uint8_t, GB_CART_FINGERPRINT_SIZE);
            if (gb_fingerprint[0] != 0) {
                rom_bank = GETDATA(curr, uint32_t);
                ram_bank = GETDATA(curr, uint32_t);
                ram_enable = GETDATA(curr, uint32_t);
                mbc1_mode = GETDATA(curr, uint32_t);
                rtc_latch = GETDATA(curr, uint32_t);
                rtc_last_time = (time_t)GETDATA(curr, int64_t);
                COPYARRAY(rtc_regs, curr, uint8_t, MBC3_RTC_REGS_COUNT);
                COPYARRAY(rtc_latched_regs, curr, uint8_t, MBC3_RTC_REGS_COUNT);
                COPYARRAY(cam_regs, curr, uint8_t, POCKET_CAM_REGS_COUNT);
            }

            if (ROM_SETTINGS.transferpak && !Controls[i].RawData) {

                /* init transferpak state if enabled and not controlled by input plugin */
                dev->transferpaks[i].enabled = enabled;
                dev->transferpaks[i].bank = bank;
                dev->transferpaks[i].access_mode = access_mode;
                dev->transferpaks[i].access_mode_changed = access_mode_changed;

                /* if it holds a valid cartridge init gbcart */
                if (dev->transferpaks[i].gb_cart != NULL
                 && dev->transferpaks[i].gb_cart->irom_storage != NULL) {
                    const uint8_t* rom = dev->transferpaks[i].gb_cart->irom_storage->data
                                        (dev->transferpaks[i].gb_cart->rom_storage);

                    /* verify that gb cart saved in savestate is the same
                     * as what is currently inserted in transferpak */
                    if (gb_fingerprint[0] != 0
                     && memcmp(gb_fingerprint,
                               rom + GB_CART_FINGERPRINT_OFFSET,
                               GB_CART_FINGERPRINT_SIZE) == 0) {

                        /* init gbcart state */
                        dev->transferpaks[i].gb_cart->rom_bank = rom_bank;
                        dev->transferpaks[i].gb_cart->ram_bank = ram_bank;
                        dev->transferpaks[i].gb_cart->ram_enable = ram_enable;
                        dev->transferpaks[i].gb_cart->mbc1_mode = mbc1_mode;
                        dev->transferpaks[i].gb_cart->rtc.latch = rtc_latch;
                        dev->transferpaks[i].gb_cart->rtc.last_time = rtc_last_time;

                        memcpy(dev->transferpaks[i].gb_cart->rtc.regs, rtc_regs, MBC3_RTC_REGS_COUNT);
                        memcpy(dev->transferpaks[i].gb_cart->rtc.latched_regs, rtc_latched_regs, MBC3_RTC_REGS_COUNT);
                        memcpy(dev->transferpaks[i].gb_cart->cam.regs, cam_regs, POCKET_CAM_REGS_COUNT);
                    }
                    else {
                        DebugMessage(M64MSG_WARNING,
                            "Savestate GB cart mismatch. Current GB cart: %s. Expected GB cart : %s",
                            (rom[GB_CART_FINGERPRINT_OFFSET] == 0x00) ? "(none)" : (const char*)(rom + GB_CART_FINGERPRINT_OFFSET),
                            (gb_fingerprint[0] == 0x00) ? "(none)" : gb_fingerprint);

                        poweron_gb_cart(dev->transferpaks[i].gb_cart);
                    }
                }
            }
        }

        /* extra pif channels state */
        for (i = 0; i < PIF_CHANNELS_COUNT; ++i) {
            int offset = GETDATA(curr, int8_t);
            if (offset >= 0) {
                setup_pif_channel(&dev->pif.channels[i], dev->pif.ram + offset);
            }
            else {
                disable_pif_channel(&dev->pif.channels[i]);
            }
        }

        /* extra si state */
        dev->si.dma_dir = GETDATA(curr, uint8_t);

        /* extra dp state */
        dev->dp.do_on_unfreeze = GETDATA(curr, uint8_t);

        /* extra vi state */
        dev->vi.count_per_scanline = GETDATA(curr, uint32_t);

        /* extra RDRAM register state */
        for (i = 1; i < RDRAM_MAX_MODULES_COUNT; ++i) {
            dev->rdram.regs[i][RDRAM_CONFIG_REG]       = GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_DEVICE_ID_REG]    = GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_DELAY_REG]        = GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_MODE_REG]         = GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_REF_INTERVAL_REG] = GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_REF_ROW_REG]      = GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_RAS_INTERVAL_REG] = GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_MIN_INTERVAL_REG] = GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_ADDR_SELECT_REG]  = GETDATA(curr, uint32_t);
            dev->rdram.regs[i][RDRAM_DEVICE_MANUF_REG] = GETDATA(curr, uint32_t);
        }

        if (version >= 0x00010500)
        {
#ifdef NEW_DYNAREC
            stop_after_jal = GETDATA(curr, uint32_t);
#else
            curr += sizeof(uint32_t);
#endif
        }
    }
    else
    {
        /* extra ai state */
        dev->ai.last_read = 0;
        dev->ai.delayed_carry = 0;

        /* extra cart_rom state */
        dev->cart.cart_rom.last_write = 0;
        dev->cart.cart_rom.rom_written = 0;

        /* extra af-rtc state */
        dev->cart.af_rtc.control = 0x200;
        dev->cart.af_rtc.now = 0;
        dev->cart.af_rtc.last_update_rtc = 0;

        /* extra controllers state */
        for(i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {
            /* skip controllers handled by the input plugin */
            if (Controls[i].RawData)
                continue;

            dev->controllers[i].flavor->reset(&dev->controllers[i]);

            if (ROM_SETTINGS.rumble) {
                poweron_rumblepak(&dev->rumblepaks[i]);
            }
            if (ROM_SETTINGS.transferpak) {
                poweron_transferpak(&dev->transferpaks[i]);
            }
        }

        /* extra pif channels state
         * HACK: Assume PIF was in channel processing mode (and not in CIC challenge mode)
         * Try to parse pif ram to setup pif channels
         */
        setup_channels_format(&dev->pif);

        /* extra vi state */
        dev->vi.count_per_scanline = (dev->vi.regs[VI_V_SYNC_REG] == 0)
            ? 1500
            : ((dev->vi.clock / dev->vi.expected_refresh_rate) / (dev->vi.regs[VI_V_SYNC_REG] + 1));

        /* extra si state */
        dev->si.dma_dir = SI_NO_DMA;

        /* extra dp state */
        dev->dp.do_on_unfreeze = 0;

        /* extra rdram state
         * Best effort, copy values from RDRAM module 0 except for DEVICE_ID
         * which is set in accordance with the IPL3 procedure with 2M modules.
         */
        for (i = 0; i < RDRAM_MAX_MODULES_COUNT; ++i) {
            memcpy(dev->rdram.regs[i], dev->rdram.regs[0], RDRAM_REGS_COUNT*sizeof(dev->rdram.regs[0][0]));
            dev->rdram.regs[i][RDRAM_DEVICE_ID_REG] = ri_address_to_id_field(i * 0x200000) << 2;
        }
    }

    /* Zilmar-Spec plugin expect a call with control_id = -1 when RAM processing is done */
    if (input.controllerCommand) {
        input.controllerCommand(-1, NULL);
    }

    /* reset fb state */
    poweron_fb(&dev->dp.fb);

    dev->sp.rsp_task_locked = 0;
    dev->r4300.cp0.interrupt_unsafe_state = 0;

    *r4300_cp0_last_addr(&dev->r4300.cp0) = *r4300_pc(&dev->r4300);

    free(savestateData);

#ifndef __LIBRETRO__
    main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "State loaded from: %s", namefrompath(filepath));
#endif
    return 1;
}

static int read_data_from_file(void *file, void *buffer, size_t length)
{
    return fread(buffer, 1, length, file) == length;
}

static savestates_type savestates_detect_type(char *filepath)
{
    unsigned char magic[4];
    FILE *f = fopen(filepath, "rb");
    if (f == NULL)
    {
        DebugMessage(M64MSG_STATUS, "Could not open state file %s\n", filepath);
        return savestates_type_unknown;
    }

    if (fread(magic, 1, 4, f) != 4)
    {
        fclose(f);
        DebugMessage(M64MSG_STATUS, "Could not read from state file %s\n", filepath);
        return savestates_type_unknown;
    }

    fclose(f);

    if (magic[0] == 0x1f && magic[1] == 0x8b) // GZIP header
        return savestates_type_m64p;
    else
    {
        DebugMessage(M64MSG_STATUS, "Unknown state file type %s\n", filepath);
        return savestates_type_unknown;
    }
}

int savestates_load(void)
{
    FILE *fPtr = NULL;
    char *filepath = NULL;
    int ret = 0;

    if (fname == NULL) // For slots, autodetect the savestate type
    {
        // try M64P type first
        type = savestates_type_m64p;
        filepath = savestates_generate_path(type);
        fPtr = fopen(filepath, "rb"); // can I open this?
	if (fPtr == NULL)
	{
		free(filepath);
		filepath = NULL;
		main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "No Mupen64Plus state file found for slot %i", slot);
		type = savestates_type_unknown;
	}
    }
    else // filename of state file to load was set explicitly in 'fname'
    {
        // detect type if unknown
        if (type == savestates_type_unknown)
        {
            type = savestates_detect_type(fname);
        }
        filepath = savestates_generate_path(type);
        if (filepath != NULL)
            fPtr = fopen(filepath, "rb"); // can I open this?
        if (fPtr == NULL)
        {
            if (filepath != NULL)
                free(filepath);
            filepath = NULL;
            main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Failed to open savestate file %s", filepath);
        }
    }
    if (fPtr != NULL)
        fclose(fPtr);

    if (filepath != NULL)
    {
        struct device* dev = &g_dev;

        switch (type)
        {
            case savestates_type_m64p: ret = savestates_load_m64p(dev, filepath); break;
            default: ret = 0; break;
        }
        free(filepath);
        filepath = NULL;
    }

    // deliver callback to indicate completion of state loading operation
    StateChanged(M64CORE_STATE_LOADCOMPLETE, ret);

    savestates_clear_job();

    return ret;
}

static void savestates_save_m64p_work(struct work_struct *work)
{
    struct savestate_work *save = container_of(work, struct savestate_work, work);

#ifdef USE_SDL
    SDL_LockMutex(savestates_lock);
#endif

#ifndef __LIBRETRO__
    // Write the state to a GZIP file
    gzFile f;
    int gzres;
    f = gzopen(save->filepath, "wb");

    if (f==NULL)
    {
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Could not open state file: %s", save->filepath);
        free(save->data);
        return;
    }

    gzres = gzwrite(f, save->data, save->size);
    if ((gzres < 0) || ((size_t)gzres != save->size))
    {
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Could not write data to state file: %s", save->filepath);
        gzclose(f);
        free(save->data);
        return;
    }

    gzclose(f);
    main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Saved state to: %s", namefrompath(save->filepath));
#else
    memcpy(save->mempointer, save->data, save->size);
#endif
    free(save->data);
#ifndef __LIBRETRO__
    free(save->filepath);
#endif
    free(save);

#ifdef USE_SDL
    SDL_UnlockMutex(savestates_lock);
#endif
}

#ifndef __LIBRETRO__
int savestates_save_m64p(const struct device* dev, char *filepath)
#else
int savestates_save_m64p(const struct device* dev, void *data)
#endif
{
    unsigned char outbuf[4];
    int i;
    uint64_t flashram_status;

    char queue[1024];

    struct savestate_work *save;
    char *curr;

    /* OK to cast away const qualifier */
    const uint32_t* cp0_regs = r4300_cp0_regs((struct cp0*)&dev->r4300.cp0);

    save = malloc(sizeof(*save));
    if (!save) {
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Insufficient memory to save state.");
        return 0;
    }

#ifndef __LIBRETRO__
    save->filepath = strdup(filepath);
#else
    save->mempointer = data;
#endif

    if(autoinc_save_slot)
        savestates_inc_slot();

    save_eventqueue_infos(&dev->r4300.cp0, queue);

    // Allocate memory for the save state data
    save->size = 16788288 + sizeof(queue) + 4 + 4096;
    save->data = curr = malloc(save->size);
    if (save->data == NULL)
    {
        free(save->filepath);
        free(save);
        main_message(M64MSG_STATUS, OSD_BOTTOM_LEFT, "Insufficient memory to save state.");
        return 0;
    }

    memset(save->data, 0, save->size);

    // Write the save state data to memory
    PUTARRAY(savestate_magic, curr, unsigned char, 8);

    outbuf[0] = (savestate_latest_version >> 24) & 0xff;
    outbuf[1] = (savestate_latest_version >> 16) & 0xff;
    outbuf[2] = (savestate_latest_version >>  8) & 0xff;
    outbuf[3] = (savestate_latest_version >>  0) & 0xff;
    PUTARRAY(outbuf, curr, unsigned char, 4);

    PUTARRAY(ROM_SETTINGS.MD5, curr, char, 32);

    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_CONFIG_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_DEVICE_ID_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_DELAY_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_MODE_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_REF_INTERVAL_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_REF_ROW_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_RAS_INTERVAL_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_MIN_INTERVAL_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_ADDR_SELECT_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_DEVICE_MANUF_REG]);

    PUTDATA(curr, uint32_t, 0); // Padding from old implementation
    PUTDATA(curr, uint32_t, dev->mi.regs[MI_INIT_MODE_REG]);
    PUTDATA(curr, uint8_t,  dev->mi.regs[MI_INIT_MODE_REG] & 0x7F);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INIT_MODE_REG] & 0x80) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INIT_MODE_REG] & 0x100) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INIT_MODE_REG] & 0x200) != 0);
    PUTDATA(curr, uint32_t, dev->mi.regs[MI_VERSION_REG]);
    PUTDATA(curr, uint32_t, dev->mi.regs[MI_INTR_REG]);
    PUTDATA(curr, uint32_t, dev->mi.regs[MI_INTR_MASK_REG]);
    PUTDATA(curr, uint32_t, 0); //Padding from old implementation
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INTR_MASK_REG] & 0x1) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INTR_MASK_REG] & 0x2) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INTR_MASK_REG] & 0x4) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INTR_MASK_REG] & 0x8) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INTR_MASK_REG] & 0x10) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INTR_MASK_REG] & 0x20) != 0);
    PUTDATA(curr, uint16_t, 0); // Padding from old implementation

    PUTDATA(curr, uint32_t, dev->pi.regs[PI_DRAM_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_CART_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_RD_LEN_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_WR_LEN_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_STATUS_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM1_LAT_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM1_PWD_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM1_PGS_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM1_RLS_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM2_LAT_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM2_PWD_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM2_PGS_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM2_RLS_REG]);

    PUTDATA(curr, uint32_t, dev->sp.regs[SP_MEM_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_DRAM_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_RD_LEN_REG]);
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_WR_LEN_REG]);
    PUTDATA(curr, uint32_t, 0); /* Padding from old implementation */
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_STATUS_REG]);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x1) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x2) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x4) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x8) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x10) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x20) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x40) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x80) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x100) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x200) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x400) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x800) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x1000) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x2000) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x4000) != 0);
    PUTDATA(curr, uint8_t, 0);
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_DMA_FULL_REG]);
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_DMA_BUSY_REG]);
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_SEMAPHORE_REG]);

    PUTDATA(curr, uint32_t, dev->sp.regs2[SP_PC_REG]);
    PUTDATA(curr, uint32_t, dev->sp.regs2[SP_IBIST_REG]);

    PUTDATA(curr, uint32_t, dev->si.regs[SI_DRAM_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->si.regs[SI_PIF_ADDR_RD64B_REG]);
    PUTDATA(curr, uint32_t, dev->si.regs[SI_PIF_ADDR_WR64B_REG]);
    PUTDATA(curr, uint32_t, dev->si.regs[SI_STATUS_REG]);

    PUTDATA(curr, uint32_t, dev->vi.regs[VI_STATUS_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_ORIGIN_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_WIDTH_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_V_INTR_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_CURRENT_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_BURST_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_V_SYNC_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_H_SYNC_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_LEAP_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_H_START_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_V_START_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_V_BURST_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_X_SCALE_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_Y_SCALE_REG]);
    PUTDATA(curr, uint32_t, dev->vi.delay);

    PUTDATA(curr, uint32_t, dev->ri.regs[RI_MODE_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_CONFIG_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_CURRENT_LOAD_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_SELECT_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_REFRESH_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_LATENCY_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_ERROR_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_WERROR_REG]);

    PUTDATA(curr, uint32_t, dev->ai.regs[AI_DRAM_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->ai.regs[AI_LEN_REG]);
    PUTDATA(curr, uint32_t, dev->ai.regs[AI_CONTROL_REG]);
    PUTDATA(curr, uint32_t, dev->ai.regs[AI_STATUS_REG]);
    PUTDATA(curr, uint32_t, dev->ai.regs[AI_DACRATE_REG]);
    PUTDATA(curr, uint32_t, dev->ai.regs[AI_BITRATE_REG]);
    PUTDATA(curr, uint32_t, dev->ai.fifo[1].duration);
    PUTDATA(curr, uint32_t    , dev->ai.fifo[1].length);
    PUTDATA(curr, uint32_t, dev->ai.fifo[0].duration);
    PUTDATA(curr, uint32_t    , dev->ai.fifo[0].length);

    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_START_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_END_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_CURRENT_REG]);
    PUTDATA(curr, uint32_t, 0); /* Padding from old implementation */
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_STATUS_REG]);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x1) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x2) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x4) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x8) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x10) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x20) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x40) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x80) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x100) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x200) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x400) != 0);
    PUTDATA(curr, uint8_t, 0);
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_CLOCK_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_BUFBUSY_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_PIPEBUSY_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_TMEM_REG]);

    PUTDATA(curr, uint32_t, dev->dp.dps_regs[DPS_TBIST_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dps_regs[DPS_TEST_MODE_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dps_regs[DPS_BUFTEST_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dps_regs[DPS_BUFTEST_DATA_REG]);

    PUTARRAY(dev->rdram.dram, curr, uint32_t, RDRAM_MAX_SIZE/4);
    PUTARRAY(dev->sp.mem, curr, uint32_t, SP_MEM_SIZE/4);
    PUTARRAY(dev->pif.ram, curr, uint8_t, PIF_RAM_SIZE);

    PUTDATA(curr, int32_t, dev->cart.use_flashram);
    PUTDATA(curr, int32_t, dev->cart.flashram.mode);
    flashram_status = ((uint64_t)dev->cart.flashram.status[0] << 32) | dev->cart.flashram.status[1];
    PUTDATA(curr, uint64_t, flashram_status);
    PUTDATA(curr, uint32_t, dev->cart.flashram.erase_offset);
    PUTDATA(curr, uint32_t, dev->cart.flashram.write_pointer);

    PUTARRAY(dev->r4300.cp0.tlb.LUT_r, curr, uint32_t, 0x100000);
    PUTARRAY(dev->r4300.cp0.tlb.LUT_w, curr, uint32_t, 0x100000);

    /* OK to cast away const qualifier */
    PUTDATA(curr, uint32_t, *r4300_llbit((struct r4300_core*)&dev->r4300));
    PUTARRAY(r4300_regs((struct r4300_core*)&dev->r4300), curr, int64_t, 32);
    PUTARRAY(cp0_regs, curr, uint32_t, CP0_REGS_COUNT);
    PUTDATA(curr, int64_t, *r4300_mult_lo((struct r4300_core*)&dev->r4300));
    PUTDATA(curr, int64_t, *r4300_mult_hi((struct r4300_core*)&dev->r4300));

    const cp1_reg *cp1_regs = r4300_cp1_regs((struct cp1*)&dev->r4300.cp1);
    PUTARRAY(&cp1_regs->dword, curr, int64_t, 32);

    PUTDATA(curr, uint32_t, *r4300_cp1_fcr0((struct cp1*)&dev->r4300.cp1));
    PUTDATA(curr, uint32_t, *r4300_cp1_fcr31((struct cp1*)&dev->r4300.cp1));
    for (i = 0; i < 32; i++)
    {
        PUTDATA(curr, int16_t, dev->r4300.cp0.tlb.entries[i].mask);
        PUTDATA(curr, int16_t, 0);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].vpn2);
        PUTDATA(curr, char, dev->r4300.cp0.tlb.entries[i].g);
        PUTDATA(curr, unsigned char, dev->r4300.cp0.tlb.entries[i].asid);
        PUTDATA(curr, int16_t, 0);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].pfn_even);
        PUTDATA(curr, char, dev->r4300.cp0.tlb.entries[i].c_even);
        PUTDATA(curr, char, dev->r4300.cp0.tlb.entries[i].d_even);
        PUTDATA(curr, char, dev->r4300.cp0.tlb.entries[i].v_even);
        PUTDATA(curr, char, 0);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].pfn_odd);
        PUTDATA(curr, char, dev->r4300.cp0.tlb.entries[i].c_odd);
        PUTDATA(curr, char, dev->r4300.cp0.tlb.entries[i].d_odd);
        PUTDATA(curr, char, dev->r4300.cp0.tlb.entries[i].v_odd);
        PUTDATA(curr, char, dev->r4300.cp0.tlb.entries[i].r);

        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].start_even);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].end_even);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].phys_even);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].start_odd);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].end_odd);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].phys_odd);
    }
    PUTDATA(curr, uint32_t, *r4300_pc((struct r4300_core*)&dev->r4300));

    PUTDATA(curr, uint32_t, *r4300_cp0_next_interrupt((struct cp0*)&dev->r4300.cp0));
    PUTDATA(curr, uint32_t, dev->vi.next_vi);
    PUTDATA(curr, uint32_t, dev->vi.field);

    to_little_endian_buffer(queue, 4, sizeof(queue)/4);
    PUTARRAY(queue, curr, char, sizeof(queue));

#ifdef NEW_DYNAREC
    PUTDATA(curr, uint32_t, using_tlb);
#else
    PUTDATA(curr, uint32_t, 0);
#endif

    PUTDATA(curr, uint32_t, dev->ai.last_read);
    PUTDATA(curr, uint32_t, dev->ai.delayed_carry);

    PUTDATA(curr, uint32_t, dev->cart.cart_rom.last_write);
    PUTDATA(curr, uint32_t, dev->cart.cart_rom.rom_written);

    PUTDATA(curr, uint32_t, 0); /* here there used to be rsp_task_locked */

    PUTDATA(curr, uint16_t, dev->cart.af_rtc.control);
    PUTDATA(curr, uint16_t, 0); /* padding to keep things aligned */
    PUTDATA(curr, int64_t, dev->cart.af_rtc.now);
    PUTDATA(curr, int64_t, dev->cart.af_rtc.last_update_rtc);

    for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {
        PUTDATA(curr, uint8_t, dev->controllers[i].status);
    }

    for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {
        PUTDATA(curr, uint8_t, dev->rumblepaks[i].state);
    }

    for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i) {
        PUTDATA(curr, uint32_t, dev->transferpaks[i].enabled);
        PUTDATA(curr, uint32_t, dev->transferpaks[i].bank);
        PUTDATA(curr, uint32_t, dev->transferpaks[i].access_mode);
        PUTDATA(curr, uint32_t, dev->transferpaks[i].access_mode_changed);

        if (dev->transferpaks[i].gb_cart == NULL) {
            uint8_t gb_fingerprint[GB_CART_FINGERPRINT_SIZE];
            memset(gb_fingerprint, 0, GB_CART_FINGERPRINT_SIZE);
            PUTARRAY(gb_fingerprint, curr, uint8_t, GB_CART_FINGERPRINT_SIZE);
        }
        else {
            uint8_t* rom = dev->transferpaks[i].gb_cart->irom_storage->data(dev->transferpaks[i].gb_cart->rom_storage);
            PUTARRAY(rom + GB_CART_FINGERPRINT_OFFSET, curr, uint8_t, GB_CART_FINGERPRINT_SIZE);

            PUTDATA(curr, uint32_t, dev->transferpaks[i].gb_cart->rom_bank);
            PUTDATA(curr, uint32_t, dev->transferpaks[i].gb_cart->ram_bank);
            PUTDATA(curr, uint32_t, dev->transferpaks[i].gb_cart->ram_enable);
            PUTDATA(curr, uint32_t, dev->transferpaks[i].gb_cart->mbc1_mode);
            PUTDATA(curr, uint32_t, dev->transferpaks[i].gb_cart->rtc.latch);
            PUTDATA(curr, int64_t, dev->transferpaks[i].gb_cart->rtc.last_time);

            PUTARRAY(dev->transferpaks[i].gb_cart->rtc.regs, curr, uint8_t, MBC3_RTC_REGS_COUNT);
            PUTARRAY(dev->transferpaks[i].gb_cart->rtc.latched_regs, curr, uint8_t, MBC3_RTC_REGS_COUNT);

            PUTARRAY(dev->transferpaks[i].gb_cart->cam.regs, curr, uint8_t, POCKET_CAM_REGS_COUNT);
        }
    }

    for (i = 0; i < PIF_CHANNELS_COUNT; ++i) {
       PUTDATA(curr, int8_t, (dev->pif.channels[i].tx == NULL)
               ? (int8_t)-1
               : (int8_t)(dev->pif.channels[i].tx - dev->pif.ram));
    }

    PUTDATA(curr, uint8_t, dev->si.dma_dir);

    PUTDATA(curr, uint8_t, dev->dp.do_on_unfreeze);

    PUTDATA(curr, uint32_t, dev->vi.count_per_scanline);

    for (i = 1; i < RDRAM_MAX_MODULES_COUNT; ++i) {
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_CONFIG_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_DEVICE_ID_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_DELAY_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_MODE_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_REF_INTERVAL_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_REF_ROW_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_RAS_INTERVAL_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_MIN_INTERVAL_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_ADDR_SELECT_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_DEVICE_MANUF_REG]);
    }

#ifdef NEW_DYNAREC
    PUTDATA(curr, uint32_t, stop_after_jal);
#else
    PUTDATA(curr, uint32_t, 0);
#endif

    init_work(&save->work, savestates_save_m64p_work);
    queue_work(&save->work);

    return 1;
}

static int write_data_to_file(void *file, const void *buffer, size_t length)
{
    return fwrite(buffer, 1, length, (FILE *)file) == length;
}

int savestates_save(void)
{
    char *filepath;
    int ret = 0;
    const struct device* dev = &g_dev;

    if (fname != NULL && type == savestates_type_unknown)
        type = savestates_type_m64p;
    else if (fname == NULL) // Always save slots in M64P format
        type = savestates_type_m64p;

    filepath = savestates_generate_path(type);
    if (filepath != NULL)
    {
        switch (type)
        {
            case savestates_type_m64p: ret = savestates_save_m64p(dev, filepath); break;
            default: ret = 0; break;
        }
        free(filepath);
    }

    // deliver callback to indicate completion of state saving operation
    StateChanged(M64CORE_STATE_SAVECOMPLETE, ret);

    savestates_clear_job();
    return ret;
}

void savestates_init(void)
{
#ifdef USE_SDL
    savestates_lock = SDL_CreateMutex();
    if (!savestates_lock) {
        DebugMessage(M64MSG_ERROR, "Could not create savestates list lock");
        return;
    }
#endif
}

void savestates_deinit(void)
{
#ifdef USE_SDL
    SDL_DestroyMutex(savestates_lock);
#endif
    savestates_clear_job();
}
