/* Nuvoton NCT6779 hardware monitor: temperatures, fan speeds and voltages,
 * read-only. Register layout from Linux drivers/hwmon/nct6775-core.c,
 * nct6775-platform.c and nct6775.h (v6.12). X79-VG4 bring-up. */
#ifndef NCT6779_H
#define NCT6779_H
#include <stdint.h>

#define NCT_TEMPS 6     /* SYSTIN, CPUTIN, AUXTIN0-3 */
#define NCT_FANS  6
#define NCT_VOLTS 15

struct nct_sensors {
    uint16_t base;                  /* HWM I/O base, 0 if not found */
    int8_t   temp_c[NCT_TEMPS];     /* degrees C; -128 = not connected */
    uint16_t fan_rpm[NCT_FANS];
    uint16_t mv[NCT_VOLTS];         /* after the chip's internal scaling */
};

/* Find the HWM logical device behind Super I/O port `sio` (0x2e or 0x4e).
 * Never enables a disabled device. 0 on success, -1 if absent or disabled. */
int nct6779_find(uint16_t sio);
/* One snapshot. The bank register is restored afterwards. 0 or -1. */
int nct6779_read(struct nct_sensors *out);
/* Labels for temp_c[] and mv[] (Linux nct6779_temp_label / in labels). */
extern const char *const nct_temp_label[NCT_TEMPS];
extern const char *const nct_volt_label[NCT_VOLTS];

#endif
