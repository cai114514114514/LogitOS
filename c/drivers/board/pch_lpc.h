/* The Intel PCH's LPC bridge (00:1f.0) and the legacy blocks behind it that a
 * board relies on before any real driver runs: the TCO watchdog, the Super I/O
 * configuration ports, and the 0xCF9 reset control register. */
#ifndef PCH_LPC_H
#define PCH_LPC_H
#include <stdint.h>

struct pch_lpc {
    uint16_t device;        /* 00:1f.0 device ID, 0 if not an Intel LPC bridge */
    uint16_t acpi_base;     /* PMBASE/ACPIBASE, 0 if ACPI decode is off */
    uint16_t lpc_en;        /* LPC I/F enables, config 0x82 */
    const char *family;
};

int pch_lpc_find(struct pch_lpc *out);

enum tco_state { TCO_ABSENT, TCO_HALTED, TCO_WAS_RUNNING, TCO_STUCK };
struct tco_report {
    enum tco_state state;
    uint16_t tcobase;
    uint16_t ticks;         /* TCO_TMR initial count, 0.6 s per tick */
    int second_timeout;     /* TCO2_STS.SECOND_TO_STS: a second timeout since the flag was last cleared (sticky across resets; NO_REBOOT can suppress the reset) */
};
/* Firmware may hand over with the TCO timer counting. Nothing in LogitOS
 * reloads it, so a running timer means a hard reset a few minutes after boot.
 * Halt it the way Linux iTCO_wdt_stop() does and report what was found. */
void tco_quiesce(const struct pch_lpc *lpc, struct tco_report *out);

#define SIO_MAX 2
struct sio_chip {
    uint16_t port;          /* 0x2e or 0x4e */
    uint16_t id;            /* chip ID registers 0x20/0x21 */
    const char *vendor;     /* "Nuvoton/Winbond", "ITE", "Fintek" */
    const char *name;       /* NULL if the ID is not in the table */
};
/* Identify Super I/O chips on the configuration ports the PCH actually
 * forwards (LPC_EN.CNF1/CNF2). Enters and always leaves configuration mode;
 * no logical device is touched. Returns the number found. */
int superio_probe(const struct pch_lpc *lpc, struct sio_chip out[SIO_MAX]);

/* 0xCF9 reset control: SYS_RST, then SYS_RST|RST_CPU|FULL_RST for a cold
 * reset. Only on an Intel PCH, where the register is defined. Returns only if
 * the reset did not take. */
void pch_reset(const struct pch_lpc *lpc);

#endif
