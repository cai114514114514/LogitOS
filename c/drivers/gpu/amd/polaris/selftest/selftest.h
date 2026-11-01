/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_POLARIS_SELFTEST_H
#define LOGIT_POLARIS_SELFTEST_H

#include <stddef.h>
#include <stdint.h>
#include "amd/polaris/desktop.h"
#include "amd/polaris/memory/arena.h"
#include "amd/polaris/resource/files.h"
#include "amd/polaris/smu/security_key.h"

/* The self-test exists only in HWHUD builds. The watched mutation compiles it
 * into a plain build too, so the host gate can show that its symbol-table check
 * ("a plain object has no self-test entry point") is able to fail. */
#if defined(LOGIT_HWHUD) || defined(POLARIS_SELFTEST_NEGCTL_ALWAYS_COMPILED)
#define POLARIS_SELFTEST_COMPILED 1
#endif

/* Stage B3 of docs/superpowers/plans/2026-11-01-rx580-stage-b.md ("security
 * key, ownership and self-test run"), design section 4 of
 * docs/superpowers/specs/2026-11-01-rx580-driver-design.md.
 *
 * What it does, in order, on a Polaris10 board whose read-only survey (stage
 * B2) already chose an arena:
 *
 *   gate      skip unless this is a Polaris10 PCI ID, the ESP has no
 *             \LOGIT\NOGPU (or NOGPU.TXT) marker, the loader did not have to
 *             assume one, the survey found an arena and the engine firmware
 *             can be sized
 *   lease     take the exclusive ownership lease (gpu_lease.h); every register
 *             write below refuses to happen without it
 *   map       map BAR5 (registers) and ONLY the arena window of BAR0, both
 *             uncached; the GOP scanout in the rest of BAR0 keeps the
 *             write-combining mapping fb_init gave it
 *   bind      bind the native platform (read-only checks of PCI config, the
 *             memory controller and the HDP)
 *   mapping   ask the platform's own mapping callback for the arena and every
 *             layout object and compare with the B2 plan before anything is
 *             written (polaris_arena_check_mapping)
 *   key       read the SMU security key: the FIRST register write of the whole
 *             run, one SMC index write (smu/security_key.h)
 *   firmware  load the SMC file the key selects plus the seven engine files
 *   smu       start the existing runtime: SMU microcode load
 *   sdma      ... then SDMA0 ring, a fill and a non-constant copy inside the
 *             arena, read back and compared
 *
 * Nothing here presents a frame: the runtime is started in
 * POLARIS_DESKTOP_MODE_SELF_TEST, so fb's native-present hook is never
 * installed and the CPU keeps drawing the desktop whatever happens. On any
 * failure the run stops at that stage and writes nothing further, leaving the
 * card as the firmware (or the last successful step) left it.
 *
 * The whole stage exists only in HWHUD builds (LOGIT_HWHUD): a plain kernel
 * has no entry point at all, which the host gate checks on the object file.
 *
 * Words used below: the SMU is the card's on-chip power-management
 * microcontroller, whose microcode must run before the engines work; the SMC is
 * its processor core, reached through the indirect register port (an index
 * register plus a data register in BAR5). BAR0 is the CPU window onto the
 * card's video memory (VRAM), BAR5 its register block. SDMA is the card's copy
 * engine; the HDP is the cache between that CPU window and VRAM, which must be
 * flushed for CPU and GPU to see each other's writes. MC addresses are the
 * card's own VRAM addresses, not CPU addresses. The arena is a private 2 MiB
 * window of VRAM chosen by stage B2 (memory/arena.h) where nothing else lives.
 *
 * Every stage prints one "[amd-polaris-runtime] stage=<name> result=<name> ..."
 * serial line; the same facts, shortened, are the HUD line
 * "gpu-rt stage=... result=... key=... smc=... sdma=...".
 * Model evidence only; not verified on X79-VG4 hardware. */

enum polaris_selftest_stage {
    POLARIS_SELFTEST_STAGE_GATE = 0,
    POLARIS_SELFTEST_STAGE_LEASE,
    POLARIS_SELFTEST_STAGE_MAP,
    POLARIS_SELFTEST_STAGE_BIND,
    POLARIS_SELFTEST_STAGE_MAPPING,
    POLARIS_SELFTEST_STAGE_KEY,
    POLARIS_SELFTEST_STAGE_FIRMWARE,
    POLARIS_SELFTEST_STAGE_SMU,
    POLARIS_SELFTEST_STAGE_SDMA,
    POLARIS_SELFTEST_STAGE_QUIESCE,
    POLARIS_SELFTEST_STAGE_COUNT
};

enum polaris_selftest_result {
    POLARIS_SELFTEST_NOT_RUN = 0,
    POLARIS_SELFTEST_OK,
    POLARIS_SELFTEST_SKIPPED_UNKNOWN_DEVICE,
    POLARIS_SELFTEST_SKIPPED_NOGPU,
    POLARIS_SELFTEST_SKIPPED_NO_ARENA,
    POLARIS_SELFTEST_SKIPPED_NO_FIRMWARE,
    POLARIS_SELFTEST_FAILED_BAD_ARGUMENT,
    POLARIS_SELFTEST_FAILED_LEASE,
    POLARIS_SELFTEST_FAILED_REGISTER_MAP,
    POLARIS_SELFTEST_FAILED_ARENA_MAP,
    POLARIS_SELFTEST_FAILED_BIND,
    POLARIS_SELFTEST_FAILED_MAPPING_CHECK,
    POLARIS_SELFTEST_FAILED_KEY_UNREADABLE,
    POLARIS_SELFTEST_FAILED_FIRMWARE,
    POLARIS_SELFTEST_FAILED_NO_MEMORY,
    POLARIS_SELFTEST_FAILED_RUNTIME_REFUSED,
    POLARIS_SELFTEST_FAILED_MEMORY_PLAN,
    POLARIS_SELFTEST_FAILED_STAGING,
    POLARIS_SELFTEST_FAILED_SMU,
    POLARIS_SELFTEST_FAILED_SDMA,
    POLARIS_SELFTEST_FAILED_SDMA_BUSY,
    POLARIS_SELFTEST_FAILED_QUARANTINED,
    POLARIS_SELFTEST_RESULT_COUNT
};

/* What the read-only survey and the resource probe established; nothing here
 * is guessed by the self-test. */
struct polaris_selftest_inputs {
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint8_t pci_revision;
    /* The B2 arena choice, or NULL when the survey did not run or refused. It
     * must stay valid for the whole run (the memory request points into it). */
    const struct polaris_arena_plan *arena_plan;
    struct polaris_memory_range vram; /* MC_VM_FB_LOCATION window, MC addresses */
    uint64_t bar0_physical;
    uint64_t bar5_physical;
    uint64_t bar5_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t pitch; /* bytes */
    /* polaris_fw_bundle_plan() size of the seven engine files; 0 when any of
     * them failed to load at probe time. The same number sized the arena. */
    uint64_t firmware_bundle_bytes;
};

/* The environment: everything the self-test needs from the kernel, so the host
 * gate can replace each piece with a model that records what was done and in
 * which order. The struct must have static storage duration: the platform the
 * runtime keeps refers back to it for the rest of the boot. */
struct polaris_selftest_ops {
    void *context;
    /* Nonzero when the boot volume carried \LOGIT\NOGPU. */
    int (*opted_out)(void *context);
    /* Take the exclusive ownership lease; 0 when held afterwards. */
    int (*acquire_lease)(void *context);
    /* Nonzero while the lease is held. Asked before EVERY register write. */
    int (*lease_held)(void *context);
    /* End the lease for good (quiesce). */
    void (*retire_lease)(void *context);
    /* Map BAR5 uncached; 0 and the CPU pointer on success. */
    int (*map_registers)(void *context, volatile uint8_t **bar5);
    /* Map BYTES of BAR0 starting at BAR_OFFSET uncached and nothing else; 0 and
     * the CPU pointer of the first mapped byte on success. */
    int (*map_arena_window)(void *context, uint64_t bar_offset, uint64_t bytes,
                            volatile uint8_t **window);
    int (*read_pci_config)(void *context, uint16_t byte_offset, uint32_t *value);
    /* One 32-bit read of BAR5 for the bind-failure diagnostics; never a write,
     * and only asked after the lease is held. 0 on success. */
    int (*read_diagnostic_register)(void *context, uint32_t byte_offset, uint32_t *value);
    /* One 32-bit write of BAR5, used only for the memory-controller aperture
     * program the owner approved (memory/gmc_aperture.h); asked only while the
     * lease is held. Optional: without it the aperture is never programmed and
     * a GOP-state card refuses at bind. 0 on success. */
    int (*write_register)(void *context, uint32_t byte_offset, uint32_t value);
    uint64_t (*now_us)(void *context);
    /* All eight firmware files for SECURITY_KEY (resource/files.h); 0 only if
     * every file is present, structurally valid and matches its pin. FILES is
     * filled even on failure so the caller can report the valid mask. */
    int (*load_firmware)(void *context, unsigned security_key,
                         struct polaris_resource_files *files);
    void (*release_firmware)(void *context, struct polaris_resource_files *files);
    void *(*allocate_workspace)(void *context, size_t bytes);
    void (*free_workspace)(void *context, void *workspace);
};

/* What the HUD and the log know afterwards. */
struct polaris_selftest_report {
    enum polaris_selftest_stage stage;   /* last stage that ran */
    enum polaris_selftest_result result; /* its outcome */
    int detail_code;                     /* stage specific error number, 0 if none */
    int key_known;
    unsigned security_key;
    const char *smc_path;                /* chosen SMC file; NULL before the key is known */
    int smu_loaded;
    uint32_t smu_load_status;
    int sdma_ran;
    uint64_t sdma_completed;
    uint64_t sdma_submitted;
    /* What the SMU loader saw in the SMC before its first write, and the
     * start-up branch it chose; known as soon as the loader has read it, so a
     * run that dies later still has it. */
    int smu_preflight_known;
    struct polaris_smu_preflight smu_preflight;
    /* The runtime got as far as configuring the SDMA ring (success, or a
     * failure at the ring stage or later): quiesce must then look at it. */
    int sdma_configured;
    /* The runtime wrote to the card and then failed or lost track of it
     * (polaris_runtime_start quarantine): the card's state is unknown. */
    unsigned runtime_quarantined;
};

/* Run the stages above. Returns POLARIS_SELFTEST_OK, or the result that
 * stopped it (skips included: a skip is not a pass). Single threaded, once per
 * boot. */
enum polaris_selftest_result
polaris_selftest_run(const struct polaris_selftest_inputs *inputs,
                     const struct polaris_selftest_ops *ops,
                     struct polaris_selftest_report *report);

/* Before reboot or S5 (dev_quiesce_all): confirm SDMA0 is idle and end the
 * lease. Read-only; returns 0 when nothing is running or the engine is idle,
 * nonzero when it could not be confirmed idle (driver.h quiesce contract).
 * The ring pointers are read whenever the runtime configured the ring, success
 * or not; a runtime that is quarantined after writing to the card is never
 * confirmed idle, because nothing can say what the card is doing. Writes
 * nothing. */
int polaris_selftest_quiesce(const struct polaris_selftest_ops *ops,
                             struct polaris_selftest_report *report);

/* "stage=... result=... key=... smc=... sdma=..." without the "gpu-rt "
 * label, for the HUD, followed by "pre=<branch> fw=... clk=... pc=... rst=...
 * fl=..." (the SMC state the loader saw, in hex) once the loader has read it. */
void polaris_selftest_format_hud(char *out, int capacity,
                                 const struct polaris_selftest_report *report);

const char *polaris_selftest_stage_name(enum polaris_selftest_stage stage);
const char *polaris_selftest_result_name(enum polaris_selftest_result result);

/* Nonzero when the boot volume carried the \LOGIT\NOGPU (or NOGPU.TXT) opt-out
 * marker, which the UEFI loader turns into a boot module of this name, or when
 * the loader could not tell and assumed it (bootmod_gpu_optout_assumed): the
 * opt-out fails closed. */
#define POLARIS_SELFTEST_OPT_OUT_MODULE "nogpu"
int polaris_selftest_opted_out(void);

#endif
