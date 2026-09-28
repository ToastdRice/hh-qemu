/*
 * QEMU model of the zmachine "zdevice" XOR/shift scratchpad
 *
 * Copyright (c) 2025 zmachine lab
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_MISC_ZDEVICE_H
#define HW_MISC_ZDEVICE_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ZDEVICE "zmachine.zdevice"
OBJECT_DECLARE_SIMPLE_TYPE(ZDeviceState, ZDEVICE)

/*
 * MMIO layout, as byte offsets from the device base address:
 *
 *   0x0000 .. 0x003f   reserved
 *   0x0040             ZDEVICE_REG_KEY   (16-bit) XOR key
 *   0x0042             ZDEVICE_REG_SHIFT (16-bit) left shift toggle
 *   0x0044 .. 0x00ff   reserved
 *   0x0100 .. 0xffff   data scratchpad
 *
 * Every halfword written to the data scratchpad is transformed before it is
 * stored:
 *
 *      stored = (written ^ KEY) << (SHIFT ? ZDEVICE_SHIFT_BITS : 0)
 *
 * A read of the scratchpad returns the stored halfword as-is, so the
 * transform is never applied twice. The two control registers are plain
 * read/write storage and are not transformed.
 *
 * Keep this in sync with the OpenSBI driver in
 * include/sbi_utils/zdevice/zdevice.h.
 */
#define ZDEVICE_SIZE            0x10000

#define ZDEVICE_CTRL_BASE       0x0040
#define ZDEVICE_REG_KEY         (ZDEVICE_CTRL_BASE + 0x0)
#define ZDEVICE_REG_SHIFT       (ZDEVICE_CTRL_BASE + 0x2)

#define ZDEVICE_DATA_BASE       0x0100

/* Bits the device shifts left by when the shift toggle is active */
#define ZDEVICE_SHIFT_BITS      2

/* Halfwords of scratchpad behind the window */
#define ZDEVICE_DATA_WORDS      ((ZDEVICE_SIZE - ZDEVICE_DATA_BASE) / 2)

struct ZDeviceState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    uint16_t key;
    uint16_t shift;
    uint16_t data[ZDEVICE_DATA_WORDS];
};

/*
 * Create a zdevice and map its window at @addr.
 */
DeviceState *zdevice_create(hwaddr addr);

#endif /* HW_MISC_ZDEVICE_H */
