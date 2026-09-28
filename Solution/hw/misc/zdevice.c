/*
 * QEMU model of the zmachine "zdevice" XOR/shift scratchpad
 *
 * Copyright (c) 2025 zmachine lab
 *
 * A 64 KiB MMIO window holding two control registers and a halfword
 * scratchpad. A halfword written to the scratchpad is stored as
 *
 *      (written ^ key) << (shift ? 2 : 0)
 *
 * and read back untransformed, so a value is never transformed twice.
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

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/misc/zdevice.h"
#include "migration/vmstate.h"

/*
 * The transform the device applies on a scratchpad write. The assignment
 * back to a uint16_t is what drops the top two bits when the shift is on,
 * so 0xffff ^ 0x0000 is stored as 0xfffc rather than as an 18-bit value.
 */
static uint16_t zdevice_transform(ZDeviceState *s, uint16_t val)
{
    uint16_t out = val ^ s->key;

    if (s->shift) {
        out <<= ZDEVICE_SHIFT_BITS;
    }

    return out;
}

static uint64_t zdevice_read(void *opaque, hwaddr addr, unsigned int size)
{
    ZDeviceState *s = opaque;

    switch (addr) {
    case ZDEVICE_REG_KEY:
        return s->key;
    case ZDEVICE_REG_SHIFT:
        return s->shift;
    default:
        break;
    }

    if (addr >= ZDEVICE_DATA_BASE) {
        return s->data[(addr - ZDEVICE_DATA_BASE) / sizeof(uint16_t)];
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: read from reserved offset 0x%" HWADDR_PRIx "\n",
                  __func__, addr);
    return 0;
}

static void zdevice_write(void *opaque, hwaddr addr, uint64_t val64,
                          unsigned int size)
{
    ZDeviceState *s = opaque;
    uint16_t val = val64;

    switch (addr) {
    case ZDEVICE_REG_KEY:
        s->key = val;
        return;
    case ZDEVICE_REG_SHIFT:
        s->shift = val;
        return;
    default:
        break;
    }

    if (addr >= ZDEVICE_DATA_BASE) {
        s->data[(addr - ZDEVICE_DATA_BASE) / sizeof(uint16_t)] =
            zdevice_transform(s, val);
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: write to reserved offset 0x%" HWADDR_PRIx
                  " (val 0x%04x)\n", __func__, addr, val);
}

static const MemoryRegionOps zdevice_ops = {
    .read = zdevice_read,
    .write = zdevice_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * The key is two bytes wide, so the halfword is the only access size
     * the device implements. A word access is split by the memory core into
     * the two halfword accesses the guest meant; a byte access is refused,
     * which on this machine surfaces as a load/store access fault. Allowing
     * it instead would turn a byte write into a read-modify-write of a whole
     * transformed halfword, and would hand a byte read the wrong half of it
     * at odd offsets.
     */
    .valid = {
        .min_access_size = 2,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 2,
        .max_access_size = 2,
    },
};

static void zdevice_reset_enter(Object *obj, ResetType type)
{
    ZDeviceState *s = ZDEVICE(obj);

    /* Come up with the identity transform and a clear scratchpad */
    s->key = 0;
    s->shift = 0;
    memset(s->data, 0, sizeof(s->data));
}

static void zdevice_init(Object *obj)
{
    ZDeviceState *s = ZDEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &zdevice_ops, s,
                          TYPE_ZDEVICE, ZDEVICE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription zdevice_vmstate = {
    .name = TYPE_ZDEVICE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(key, ZDeviceState),
        VMSTATE_UINT16(shift, ZDeviceState),
        VMSTATE_UINT16_ARRAY(data, ZDeviceState, ZDEVICE_DATA_WORDS),
        VMSTATE_END_OF_LIST()
    }
};

static void zdevice_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = zdevice_reset_enter;
    dc->vmsd = &zdevice_vmstate;
}

static const TypeInfo zdevice_info = {
    .name          = TYPE_ZDEVICE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ZDeviceState),
    .instance_init = zdevice_init,
    .class_init    = zdevice_class_init,
};

static void zdevice_register_types(void)
{
    type_register_static(&zdevice_info);
}

type_init(zdevice_register_types)

DeviceState *zdevice_create(hwaddr addr)
{
    DeviceState *dev = qdev_new(TYPE_ZDEVICE);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);

    return dev;
}
