/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - rom.c                                                   *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2008 Tillin9                                            *
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

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#define M64P_CORE_PROTOTYPES 1
#include "api/callbacks.h"
#include "api/config.h"
#include "api/m64p_config.h"
#include "api/m64p_types.h"
#include "device/device.h"
#include "main.h"
#include "osal/preproc.h"
#include "osd/osd.h"
#include "rom.h"
#include "rom_settings.h"
#include "util.h"

/* Number of cpu cycles per instruction */
enum { DEFAULT_COUNT_PER_OP = 2 };
/* by default, extra mem is enabled */
enum { DEFAULT_DISABLE_EXTRA_MEM = 0 };
/* Default SI DMA duration */
enum { DEFAULT_SI_DMA_DURATION = 0x900 };

/* Global loaded rom size. */
size_t g_rom_size = 0;

m64p_rom_header   ROM_HEADER;
rom_params        ROM_PARAMS;
m64p_rom_settings ROM_SETTINGS;

static m64p_system_type rom_country_code_to_system_type(uint16_t country_code);

static const uint8_t Z64_SIGNATURE[4] = { 0x80, 0x37, 0x12, 0x40 };
static const uint8_t V64_SIGNATURE[4] = { 0x37, 0x80, 0x40, 0x12 };
static const uint8_t N64_SIGNATURE[4] = { 0x40, 0x12, 0x37, 0x80 };

/* Tests if a file is a valid N64 rom by checking the first 4 bytes. */
static int is_valid_rom(const unsigned char *buffer)
{
    if (memcmp(buffer, Z64_SIGNATURE, sizeof(Z64_SIGNATURE)) == 0
     || memcmp(buffer, V64_SIGNATURE, sizeof(V64_SIGNATURE)) == 0
     || memcmp(buffer, N64_SIGNATURE, sizeof(N64_SIGNATURE)) == 0)
        return 1;
    else
        return 0;
}

int compare_entry(const void *in1, const void *in2)
{
    const uint64_t *c1 = in1;
    const uint64_t *c2 = in2;
    int64_t rem;

    bool overflow = __builtin_sub_overflow(*c1, *c2, &rem);
    if(overflow || rem < 0)
        return -1;
    else if(rem > 0)
        return 1;

    return 0;
}

int get_rom_settings(uint64_t crc, romdatabase_entry *entry)
{
    size_t nmemb = sizeof(rom_crc)/sizeof(*rom_crc);
    const uint64_t *const crc_start = rom_crc;
    uint64_t *crc_ret;
    const struct rom_entry_s *r;

    crc_ret = bsearch(&crc, rom_crc, nmemb, sizeof(*rom_crc), compare_entry);
    if(crc_ret == NULL)
        return -1;

    DebugMessage(M64MSG_VERBOSE, "Special settings were found for this ROM.");

    r = &rom_dat[crc_ret - crc_start];
    entry->status = r->status;
    entry->savetype = r->save_type;
    entry->players = r->players;
    entry->rumble = r->rumble;
    entry->countperop = r->count_per_op;
    entry->disableextramem = r->disable_extra_mem;
    entry->transferpak = r->transferpak;
    entry->mempak = r->mempak;
    entry->biopak = r->biopak;
    entry->sidmaduration = r->si_dma_duration ? 0x100 : 0x900;
    entry->cheats = cheats[r->cheat_lut];

    return 0;
}

/* Copies the source block of memory to the destination block of memory while
 * switching the endianness of .v64 and .n64 images to the .z64 format, which
 * is native to the Nintendo 64. The data extraction routines and MD5 hashing
 * function may only act on the .z64 big-endian format.
 *
 * IN: src: The source block of memory. This must be a valid Nintendo 64 ROM
 *          image of 'len' bytes.
 *     len: The length of the source and destination, in bytes.
 * OUT: dst: The destination block of memory. This must be a valid buffer for
 *           at least 'len' bytes.
 *      imagetype: A pointer to a byte that gets updated with the value of
 *                 V64IMAGE, N64IMAGE or Z64IMAGE according to the format of
 *                 the source block. The value is undefined if 'src' does not
 *                 represent a valid Nintendo 64 ROM image.
 */
/* FIXME: Do not copy z64 images. */
static void swap_copy_rom(void* dst, const void* src, size_t len, image_type* imagetype)
{
    if (memcmp(src, V64_SIGNATURE, sizeof(V64_SIGNATURE)) == 0)
    {
        size_t i;
        const uint16_t* src16 = (const uint16_t*) src;
        uint16_t* dst16 = (uint16_t*) dst;

        *imagetype = V64IMAGE;
        /* .v64 images have byte-swapped half-words (16-bit). */
        for (i = 0; i < len; i += 2)
        {
            *dst16++ = m64p_swap16(*src16++);
        }
    }
    else if (memcmp(src, N64_SIGNATURE, sizeof(N64_SIGNATURE)) == 0)
    {
        size_t i;
        const uint32_t* src32 = (const uint32_t*) src;
        uint32_t* dst32 = (uint32_t*) dst;

        *imagetype = N64IMAGE;
        /* .n64 images have byte-swapped words (32-bit). */
        for (i = 0; i < len; i += 4)
        {
            *dst32++ = m64p_swap32(*src32++);
        }
    }
    else {
        *imagetype = Z64IMAGE;
        memcpy(dst, src, len);
    }
}

m64p_error open_rom(const uint8_t *const romimage, size_t size)
{
    romdatabase_entry entry;
    image_type imgtype;
    uint64_t crc64;
    const char *const image_string[] = {
        ".z64 (native)", ".v64 (byteswapped)", ".n64 (byteswapped)"
    };

    /* check input requirements */
    if (romimage == NULL || !is_valid_rom(romimage))
    {
        DebugMessage(M64MSG_ERROR, "%s: not a valid ROM image", __func__);
        return M64ERR_INPUT_INVALID;
    }

    /* Clear Byte-swapped flag, since ROM is now deleted. */
    g_RomWordsLittleEndian = 0;
    /* allocate new buffer for ROM and copy into this buffer */
    g_rom_size = size;
    swap_copy_rom((uint8_t*)mem_base_u32(g_mem_base, MM_CART_ROM), romimage, size, &imgtype);
    /* ROM is now in N64 native (big endian) byte order */

    memcpy(&ROM_HEADER, (uint8_t*)mem_base_u32(g_mem_base, MM_CART_ROM), sizeof(m64p_rom_header));
    crc64 = tohl(ROM_HEADER.CRC1);
    crc64 <<= 32;
    crc64 |= tohl(ROM_HEADER.CRC2);

    memcpy(ROM_PARAMS.headername, ROM_HEADER.Name, 20);
    ROM_PARAMS.headername[20] = '\0';

    /* FIXME: Skip the middleman: set ROM_SETTINGS directly. */
    if(get_rom_settings(crc64, &entry) == 0)
    {
        ROM_SETTINGS.savetype = entry.savetype;
        ROM_SETTINGS.status = entry.status;
        ROM_SETTINGS.players = entry.players;
        ROM_SETTINGS.rumble = entry.rumble;
        ROM_SETTINGS.transferpak = entry.transferpak;
        ROM_SETTINGS.mempak = entry.mempak;
        ROM_SETTINGS.biopak = entry.biopak;
        ROM_PARAMS.countperop = entry.countperop;
        ROM_PARAMS.disableextramem = entry.disableextramem;
        ROM_PARAMS.sidmaduration = entry.sidmaduration;
        ROM_PARAMS.cheats = entry.cheats;
    }
    else
    {
        ROM_SETTINGS.savetype = NONE;
        ROM_SETTINGS.status = 0;
        ROM_SETTINGS.players = 4;
        ROM_SETTINGS.rumble = 1;
        ROM_SETTINGS.transferpak = 0;
        ROM_SETTINGS.mempak = 1;
        ROM_SETTINGS.biopak = 0;
        ROM_PARAMS.countperop = DEFAULT_COUNT_PER_OP;
        ROM_PARAMS.disableextramem = DEFAULT_DISABLE_EXTRA_MEM;
        ROM_PARAMS.sidmaduration = DEFAULT_SI_DMA_DURATION;
        ROM_PARAMS.cheats = NULL;
    }

    ROM_PARAMS.systemtype = rom_country_code_to_system_type(ROM_HEADER.Country_code);

    /* print out a bunch of info about the ROM */
    DebugMessage(M64MSG_INFO, "Name: %s", ROM_HEADER.Name);
    DebugMessage(M64MSG_INFO, "CRC: %08" PRIX64 " %08" PRIX64,
		    crc64 >> 32, crc64 & 0xFFFFFFFF);
    DebugMessage(M64MSG_INFO, "Imagetype: %s", image_string[imgtype]);
    DebugMessage(M64MSG_INFO, "Rom size: %zu bytes (%zu MiB)", g_rom_size, g_rom_size/1024/1024);
    DebugMessage(M64MSG_VERBOSE, "ClockRate = %" PRIX32, tohl(ROM_HEADER.ClockRate));
    DebugMessage(M64MSG_INFO, "Version: %" PRIX32, tohl(ROM_HEADER.Release));
    if(tohl(ROM_HEADER.Manufacturer_ID) == 'N')
        DebugMessage(M64MSG_INFO, "Manufacturer: Nintendo");
    else
        DebugMessage(M64MSG_INFO, "Manufacturer: %" PRIX32, tohl(ROM_HEADER.Manufacturer_ID));
    DebugMessage(M64MSG_VERBOSE, "Cartridge_ID: %" PRIX16, ROM_HEADER.Cartridge_ID);
    DebugMessage(M64MSG_INFO, "Country: %s", countrycodestring(ROM_HEADER.Country_code));
    DebugMessage(M64MSG_VERBOSE, "PC = %" PRIX32, tohl(ROM_HEADER.PC));
    DebugMessage(M64MSG_VERBOSE, "Save type: %d", ROM_SETTINGS.savetype);

    return M64ERR_SUCCESS;
}

m64p_error close_rom(void)
{
    /* Clear Byte-swapped flag, since ROM is now deleted. */
    g_RomWordsLittleEndian = 0;
    DebugMessage(M64MSG_STATUS, "Rom closed.");

    return M64ERR_SUCCESS;
}

/********************************************************************************************/
/* ROM utility functions */

// Get the system type associated to a ROM country code.
static m64p_system_type rom_country_code_to_system_type(uint16_t country_code)
{
    switch (country_code & UINT16_C(0xFF))
    {
        // PAL codes
        case 0x44:
        case 0x46:
        case 0x49:
        case 0x50:
        case 0x53:
        case 0x55:
        case 0x58:
        case 0x59:
            return SYSTEM_PAL;

        // NTSC codes
        case 0x37:
        case 0x41:
        case 0x45:
        case 0x4a:
        default: // Fallback for unknown codes
            return SYSTEM_NTSC;
    }
}
