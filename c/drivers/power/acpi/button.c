/* The front-panel power button: ACPI fixed event or PNP0C0C Notify 0x80 ->
 * an orderly kernel_poweroff(). X79-VG4 phase 1, task T8.
 * Model and QEMU evidence only; not verified on X79-VG4 hardware. */
#include <stdint.h>
#include <uacpi/event.h>
#include <uacpi/notify.h>
#include <uacpi/namespace.h>
#include <uacpi/utilities.h>
#include <uacpi/status.h>
#include "button.h"
#include "kprintf.h"
#include "power.h"
#include "sched.h"
#include "work.h"

/* ---- power button --------------------------------------------------------
 * The SCI handler may not sleep, and kernel_poweroff() drains the filesystems,
 * which waits on I/O the kworker itself may be completing. So the interrupt
 * queues a work item, and the work item starts a thread that does the
 * shutdown. A second press while one is in flight is ignored. */
static struct work g_press_work;
static int g_pressed;

static void shutdown_thread(void)
{
    kprintf("[board] power button: shutting down\n");
    int rc = kernel_poweroff();
    kprintf("[board] power button: poweroff returned %d\n", rc);
    __atomic_store_n(&g_pressed, 0, __ATOMIC_RELEASE);
    thread_exit();
}

static void press_work(void *arg)
{
    (void)arg;
    if (thread_create(shutdown_thread, "pwrbtn") < 0) {
        kprintf("[board] power button: no thread for the shutdown\n");
        __atomic_store_n(&g_pressed, 0, __ATOMIC_RELEASE);
    }
}

static void press(void)
{
    if (__atomic_exchange_n(&g_pressed, 1, __ATOMIC_ACQ_REL)) return;
    work_queue(&g_press_work);
}

static uacpi_interrupt_ret on_fixed(uacpi_handle ctx)
{
    (void)ctx;
    press();
    return UACPI_INTERRUPT_HANDLED;
}

static uacpi_status on_notify(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value)
{
    (void)ctx; (void)node;
    if (value == 0x80) press();          /* ACPI 6.5 table 5.186: button pressed */
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision on_button_device(void *ctx, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)depth;
    if (uacpi_install_notify_handler(node, on_notify, NULL) == UACPI_STATUS_OK)
        (*(int *)ctx)++;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

int power_acpi_button_install(void)
{
    work_item_init(&g_press_work, press_work, NULL);
    /* Firmware publishes the button either as the PM1 fixed event or as a
     * PNP0C0C control-method device (FADT PWR_BUTTON says which, and cheap
     * firmware gets that flag wrong), so arm both; press() debounces. */
    /* PWRBTN_STS latches whether or not the event is enabled and survives a
     * CF9 reset (Intel 6 Series/C200 datasheet, PM1_STS), and installing the
     * handler enables the event at once: clear a press from before this
     * point, or a button pushed during POST powers the machine straight off. */
#ifndef BOARD_NEGCTL_PWRBTN_KEEP_STALE
    uacpi_clear_fixed_event(UACPI_FIXED_EVENT_POWER_BUTTON);
#endif
    uacpi_status fixed = uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, on_fixed, NULL);
    int devices = 0;
    uacpi_find_devices("PNP0C0C", on_button_device, &devices);
    /* A control-method button notifies from GPE AML, and no GPE without a
     * driver of its own runs until this is called once (uacpi/event.h). Not
     * exercised under QEMU, whose button is the fixed event. */
    uacpi_status gpe = uacpi_finalize_gpe_initialization();
    kprintf("[board] power button: fixed=%s control-method devices=%d gpe-init=%s\n",
            fixed == UACPI_STATUS_OK ? "armed" : uacpi_status_to_string(fixed), devices,
            uacpi_status_to_string(gpe));
    return fixed == UACPI_STATUS_OK || devices ? 0 : -1;
}

