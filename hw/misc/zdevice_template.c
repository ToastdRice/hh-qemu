/*
 * QEMU model of the zmachine "zdevice" XOR/shift scratchpad - blank template
 *
 * Copyright (c) 2025 zmachine lab
 *
 * A starting point for hw/misc/zdevice.c: copy it over that file, since this
 * one is not built. It builds and boots as it is - the device registers
 * itself and maps its 64 KiB window where the zmachine board asks for it -
 * but every read returns 0 and every write is dropped, so OpenSBI's zdevice
 * self test fails and the kernel prints nothing but '?'. Fill in the TODOs
 * until both come out right. Run with "-d unimp" to watch the accesses that
 * are still unhandled.
 *
 * The device is specified in docs/platform/zmachine.md in the OpenSBI tree.
 * include/hw/misc/zdevice.h declares the device state and what the board
 * expects from this file.
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

static uint64_t zdevice_read(void *opaque, hwaddr addr, unsigned int size)
{
    /*
     * TODO: @opaque is the ZDeviceState and @addr is the byte offset into
     * the window. Return what the guest reads there.
     */
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented read of %u bytes at 0x%"
                  HWADDR_PRIx "\n", __func__, size, addr);
    return 0;
}

static void zdevice_write(void *opaque, hwaddr addr, uint64_t val64,
                          unsigned int size)
{
    /*
     * TODO: @opaque is the ZDeviceState and @addr is the byte offset into
     * the window. Do whatever writing @val64 there should do.
     */
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented write of %u bytes at 0x%"
                  HWADDR_PRIx " (val 0x%" PRIx64 ")\n",
                  __func__, size, addr, val64);
}

static const MemoryRegionOps zdevice_ops = {
    // set the memory region read/write callbacks
    // specify endianness
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
    /* TODO: create the state structure and initialize/zero all values */
}

static void zdevice_init(Object *obj)
{
    ZDeviceState *s = ZDEVICE(obj);

    // TODO: initialize the memory mapping for IO use

    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void zdevice_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = zdevice_reset_enter;
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
