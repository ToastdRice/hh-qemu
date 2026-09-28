/*
 * QEMU model of the "zmachine" bare-minimum RISC-V machine
 *
 *   - one rv32 hart,
 *   - RAM at 0x80000000,
 *   - a SiFive-style CLINT (the ACLINT MSWI and MTIMER devices),
 *   - one 8250 UART
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

#define ZMACHINE_CLINT_BASE     0x02000000
#define ZMACHINE_UART_BASE      0x10000000
#define ZMACHINE_DRAM_BASE      0x80000000

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

static void zmachine_cpu_reset(void *opaque)
{
    RISCVCPU *cpu = opaque;

    cpu->env.gpr[10] = cpu->env.mhartid;    /* a0 */
    cpu->env.gpr[11] = 0;                   /* a1 */
}

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

    // TODO: initialize a new chunk of RAM with a subregion
    //  that is 8 byte aligned around where our kernel will be injected

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


    riscv_aclint_swi_create(ZMACHINE_CLINT_BASE, 0, 1, false);
    riscv_aclint_mtimer_create(ZMACHINE_CLINT_BASE + RISCV_ACLINT_SWI_SIZE,
                               RISCV_ACLINT_DEFAULT_MTIMER_SIZE, 0, 1,
                               RISCV_ACLINT_DEFAULT_MTIMECMP,
                               RISCV_ACLINT_DEFAULT_MTIME,
                               ZMACHINE_TIMEBASE_FREQ, true);

    serial_mm_init(system_memory, ZMACHINE_UART_BASE, 0, NULL,
                   ZMACHINE_UART_BAUDBASE, serial_hd(0),
                   DEVICE_LITTLE_ENDIAN);

    // TODO: create the zdevice

    riscv_boot_info_init(&boot_info, &s->cpus);
    zmachine_load_firmware(machine, &boot_info);

    // TODO: This is not where we want to be loading the kernel from...
    if (machine->kernel_filename) {
        riscv_load_kernel(machine, &boot_info, 0x80200000, false,
                          NULL);

        if (boot_info.image_low_addr != 0x80200000) {
            error_report("kernel starts at 0x%" HWADDR_PRIx
                         ", but the firmware jumps to 0x%x",
                         boot_info.image_low_addr, 0x80200000);
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

    mc->desc = "RISC-V zmachine (CLINT and 8250 UART)";
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
