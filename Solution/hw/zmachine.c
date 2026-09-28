/*
 * QEMU model of the "zmachine" bare-minimum RISC-V machine
 *
 * Copyright (c) 2025 zmachine lab
 *
 * This machine exists to run one specific firmware: the OpenSBI "zmachine"
 * platform (opensbi/platform/zmachine) and whatever S-mode payload it jumps
 * to. It therefore models exactly what that firmware touches and nothing
 * else:
 *
 *   - one rv32 hart,
 *   - RAM at 0x80000000,
 *   - 1 MiB of payload RAM at 0xa0000000, which the S-mode kernel runs from,
 *   - a SiFive-style CLINT (the ACLINT MSWI and MTIMER devices),
 *   - one 8250 UART,
 *   - the zdevice XOR/shift scratchpad.
 *
 * There is no interrupt controller: nothing here raises an external
 * interrupt, so the UART's interrupt line is left unconnected. There is no
 * boot ROM either - the hart resets straight into the firmware - and no
 * device tree: the OpenSBI zmachine platform carries its own DTB (see
 * FW_FDT_PATH in that platform's objects.mk) and overrides whatever a1 holds
 * with it, so a tree generated here would only be thrown away.
 *
 * The addresses below must agree with zmachine.dts and with the constants in
 * the OpenSBI platform.c.
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
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/char/serial-mm.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/misc/zdevice.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/machines-qom.h"
#include "hw/riscv/riscv_hart.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"

/* Memory map. Keep in step with zmachine.dts. */
#define ZMACHINE_CLINT_BASE     0x02000000
#define ZMACHINE_UART_BASE      0x10000000
#define ZMACHINE_ZDEVICE_BASE   0x60000000
#define ZMACHINE_DRAM_BASE      0x80000000
#define ZMACHINE_PAYLOAD_BASE   0xa0000000
#define ZMACHINE_PAYLOAD_SIZE   (1 * MiB)

/*
 * Where the firmware hands control over. OpenSBI's fw_jump does not carry
 * the S-mode payload, it just jumps to a fixed address once it is done, so
 * a -kernel that is not an ELF is loaded there, and one that is has to be
 * linked there. Keep equal to FW_JUMP_ADDR in the OpenSBI platform's
 * objects.mk and to the link address in kernel.ld.
 */
#define ZMACHINE_KERNEL_BASE    0xa000c000

/* "timebase-frequency" of the CLINT's mtime counter */
#define ZMACHINE_TIMEBASE_FREQ  10000000

/* "clock-frequency" the UART's baud rate divisor is derived from */
#define ZMACHINE_UART_BAUDBASE  3686400

#define TYPE_ZMACHINE_MACHINE MACHINE_TYPE_NAME("zmachine")
OBJECT_DECLARE_SIMPLE_TYPE(ZMachineState, ZMACHINE_MACHINE)

struct ZMachineState {
    /*< private >*/
    MachineState parent_obj;

    /*< public >*/
    RISCVHartArrayState cpus;
    MemoryRegion payload_ram;
};

/*
 * Set up the argument registers the firmware is entered with.
 *
 * A machine with a boot ROM would do this in the reset vector it puts there.
 * This one resets the hart directly into the firmware, so the values are
 * written here instead, after the CPU's own reset has cleared the way.
 *
 * a0 is the hart ID. a1 would be the address of a device tree, and is left
 * at zero: the firmware brings its own, and overrides a1 before it looks at
 * it.
 */
static void zmachine_cpu_reset(void *opaque)
{
    RISCVCPU *cpu = opaque;

    cpu->env.gpr[10] = cpu->env.mhartid;    /* a0 */
    cpu->env.gpr[11] = 0;                   /* a1 */
}

/*
 * Load the firmware at the address the hart resets to.
 *
 * There is no in-tree default that would work here - the OpenSBI binaries
 * QEMU ships are fw_dynamic images built for the "generic" platform, which
 * expect a device tree from the machine - so -bios is required rather than
 * silently defaulted.
 */
static void zmachine_load_firmware(MachineState *machine, RISCVBootInfo *info)
{
    hwaddr load_addr = ZMACHINE_DRAM_BASE;
    char *filename;

    if (!machine->firmware || !strcmp(machine->firmware, "default") ||
        !strcmp(machine->firmware, "none")) {
        error_report("the zmachine machine needs its own OpenSBI build: "
                     "pass -bios fw_jump.bin");
        exit(1);
    }

    filename = riscv_find_firmware(machine->firmware, NULL);
    riscv_load_firmware(machine, info, filename, &load_addr, NULL);
    g_free(filename);

    /*
     * The reset vector is fixed at the base of RAM, so an ELF firmware that
     * wants to live somewhere else would never be entered.
     */
    if (load_addr != ZMACHINE_DRAM_BASE) {
        error_report("firmware entry point is 0x%" HWADDR_PRIx
                     ", but this machine resets to 0x%x",
                     load_addr, ZMACHINE_DRAM_BASE);
        exit(1);
    }
}

static void zmachine_init(MachineState *machine)
{
    ZMachineState *s = ZMACHINE_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    RISCVBootInfo boot_info;

    memory_region_add_subregion(system_memory, ZMACHINE_DRAM_BASE,
                                machine->ram);

    /*
     * The S-mode kernel's own RAM, well away from the firmware. It is a
     * fixed part of the board, so -m does not move or resize it.
     */
    memory_region_init_ram(&s->payload_ram, NULL, "riscv.zmachine.payload_ram",
                           ZMACHINE_PAYLOAD_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, ZMACHINE_PAYLOAD_BASE,
                                &s->payload_ram);

    /*
     * The one hart, resetting straight into the firmware.
     *
     * The default CPU has more than the rv32imac the device tree advertises,
     * which costs nothing: the firmware is built for the smaller ISA and
     * simply does not use the rest.
     */
    object_initialize_child(OBJECT(machine), "cpus", &s->cpus,
                            TYPE_RISCV_HART_ARRAY);
    object_property_set_str(OBJECT(&s->cpus), "cpu-type", machine->cpu_type,
                            &error_abort);
    object_property_set_int(OBJECT(&s->cpus), "num-harts", 1, &error_abort);
    object_property_set_int(OBJECT(&s->cpus), "resetvec", ZMACHINE_DRAM_BASE,
                            &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&s->cpus), &error_fatal);

    /* Registered after the harts so that it runs after their own reset */
    qemu_register_reset(zmachine_cpu_reset, &s->cpus.harts[0]);

    /*
     * The CLINT: software interrupts at the base of the window and the timer
     * right behind them, which is the layout OpenSBI's CLINT_MSWI_OFFSET and
     * CLINT_MTIMER_OFFSET describe.
     */
    riscv_aclint_swi_create(ZMACHINE_CLINT_BASE, 0, 1, false);
    riscv_aclint_mtimer_create(ZMACHINE_CLINT_BASE + RISCV_ACLINT_SWI_SIZE,
                               RISCV_ACLINT_DEFAULT_MTIMER_SIZE, 0, 1,
                               RISCV_ACLINT_DEFAULT_MTIMECMP,
                               RISCV_ACLINT_DEFAULT_MTIME,
                               ZMACHINE_TIMEBASE_FREQ, true);

    /* The console. Polled, so its interrupt line goes nowhere. */
    serial_mm_init(system_memory, ZMACHINE_UART_BASE, 0, NULL,
                   ZMACHINE_UART_BAUDBASE, serial_hd(0),
                   DEVICE_LITTLE_ENDIAN);

    zdevice_create(ZMACHINE_ZDEVICE_BASE);

    riscv_boot_info_init(&boot_info, &s->cpus);
    zmachine_load_firmware(machine, &boot_info);

    /*
     * The S-mode payload, if there is one. No initrd: nothing on this
     * machine describes one to the payload, so loading it would only put
     * bytes in RAM that nobody can find.
     */
    if (machine->kernel_filename) {
        riscv_load_kernel(machine, &boot_info, ZMACHINE_KERNEL_BASE, false,
                          NULL);

        /*
         * OpenSBI jumps to ZMACHINE_KERNEL_BASE no matter where the ELF
         * went, so a kernel linked anywhere else would just hang silently.
         */
        if (boot_info.image_low_addr != ZMACHINE_KERNEL_BASE) {
            error_report("kernel starts at 0x%" HWADDR_PRIx
                         ", but the firmware jumps to 0x%x",
                         boot_info.image_low_addr, ZMACHINE_KERNEL_BASE);
            exit(1);
        }
    }
}

static void zmachine_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        TYPE_RISCV_CPU_BASE32,
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V zmachine (CLINT, 8250 UART and zdevice only)";
    mc->init = zmachine_init;
    mc->min_cpus = 1;
    mc->default_cpus = 1;
    mc->max_cpus = 1;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE32;
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_id = "riscv.zmachine.ram";
    mc->default_ram_size = 128 * MiB;
    mc->no_cdrom = true;
    mc->no_floppy = true;
    mc->no_parallel = true;
}

static const TypeInfo zmachine_machine_types[] = {
    {
        .name          = TYPE_ZMACHINE_MACHINE,
        .parent        = TYPE_MACHINE,
        .instance_size = sizeof(ZMachineState),
        .class_init    = zmachine_machine_class_init,
        .interfaces    = riscv32_machine_interfaces,
    }
};

DEFINE_TYPES(zmachine_machine_types)
