#ifndef LOGIT_DEVSSH_H
#define LOGIT_DEVSSH_H

#include <stdint.h>

/* DEVELOPER SSH CHANNEL FOR BRING-UP BUILDS (HWHUD=1, the macro LOGIT_HWHUD).
 *
 * WHY THIS EXISTS. On the X79-VG4 the USB keyboard does not work yet, so
 * nothing on that machine can be typed into, and the product image is built so
 * that a person cannot reach it from the network without typing first: it
 * ships no accounts, no host keys and sshd is off (c/apps/coreutils/
 * boot_services.h starts /bin/sshd only when a root-owned /etc/sshd.enabled
 * exists; sshd authenticates against /etc/passwd and a per-account
 * ~/.ssh/authorized_keys, c/apps/services/ssh/sshd.c find_account and
 * check_authorized_key). This file lets the person building the bring-up USB
 * stick opt in to a key-only login for ONE account, without touching the
 * product behaviour and without anybody typing on the board.
 *
 * THE MECHANISM, in one paragraph. The bring-up stick carries \LOGIT\DEVSSH.PUB
 * (tools/mkesp.py --file). The UEFI loader (c/boot/efi/loader.c, itself built
 * with LOGIT_HWHUD for this) reads it before ExitBootServices and hands it to
 * the kernel as the boot module DEVSSH_MODULE_NAME, exactly as stage B0 hands
 * over the AMD microcode (c/kernel/init/bootmod.c). After the root filesystem
 * is mounted and before init starts /bin/login, devssh_provision_boot() checks
 * the module and writes the three things the EXISTING user space looks for:
 *   - one /etc/passwd row for the account "dev" whose hash field can never
 *     verify, so no password opens it;
 *   - /home/dev/.ssh/authorized_keys holding exactly the module's keys;
 *   - /etc/sshd.enabled, created LAST so sshd never starts before the keys
 *     are in place.
 *
 * WHY THE KERNEL DOES IT. The board's root partition was installed from
 * Windows long ago and is NOT updated by the stick (docs/hw/X79-VG4.md): the
 * stick carries only the loader and the kernel. /bin/login, /bin/sshd and the
 * account code on that root are the OLD user space, so the kernel writes the
 * files in the format those old binaries read, and no user-space file is
 * changed. accounts.h, sshd.c, login.c and boot_services.h last changed
 * 2026-08-09 .. 2026-09-22, before that root image existed (2026-10-31), so
 * the format below is the one the installed binaries parse.
 *
 * WHAT IT IS NOT. It is not part of any product build: without LOGIT_HWHUD this
 * header reduces to an empty inline function and devssh.c compiles to nothing
 * (tests/hw/devssh/inert_test.c and the nm check in tests/hw-devssh.mk). It is
 * not a way to log in with a password: the row's hash is "!", which pwhash_check
 * (c/crypto/kdf/pbkdf2.c) rejects because it is not a "$pbkdf2-sha256$" record.
 * It is not removed again when the stick is replaced: a boot without the module
 * changes nothing (the files stay on the root partition until it is
 * re-imaged). Model and QEMU evidence only; not verified on X79-VG4 hardware. */

/* The boot-module name the loader registers for \LOGIT\DEVSSH.PUB. */
#define DEVSSH_MODULE_NAME        "devssh-authorized-keys"
/* The largest module the loader accepts and the validator reads. 4 KiB holds
 * dozens of Ed25519 lines (about 100 bytes each) and keeps a corrupt FAT
 * directory entry from becoming a large allocation. */
#define DEVSSH_MODULE_BYTES_MAX   4096

/* The account the channel logs in as, and where its files live. */
#define DEVSSH_ACCOUNT_NAME       "dev"
#define DEVSSH_HOME_PATH          "/home/dev"
#define DEVSSH_SSH_DIR_PATH       "/home/dev/.ssh"
#define DEVSSH_AUTHKEYS_PATH      "/home/dev/.ssh/authorized_keys"
#define DEVSSH_STORE_PATH         "/etc/passwd"
#define DEVSSH_ENABLE_FLAG_PATH   "/etc/sshd.enabled"

#if defined(LOGIT_HWHUD) || defined(DEVSSH_HOSTTEST)

/* Why a module was refused. The values are stable so the host test can name
 * each case. */
enum devssh_verdict {
    DEVSSH_OK = 0,
    DEVSSH_EMPTY,        /* no bytes at all */
    DEVSSH_TOO_BIG,      /* more than DEVSSH_MODULE_BYTES_MAX */
    DEVSSH_BAD_BYTE,     /* NUL, control byte, or a byte above 0x7e */
    DEVSSH_BAD_LINE,     /* a line that is not "ssh-ed25519 <base64> [comment]" */
    DEVSSH_BAD_TYPE,     /* a key type other than ssh-ed25519 */
    DEVSSH_BAD_BASE64,   /* the key field is not 68 base64 characters */
    DEVSSH_BAD_BLOB,     /* decodes, but is not an Ed25519 public-key blob */
    DEVSSH_NO_KEY        /* only blank lines */
};

/* The validated, normalised key list: one "ssh-ed25519 <base64>[ comment]\n"
 * line per key, CRs and blank lines removed. NUL terminated. This, byte for
 * byte, is what authorized_keys receives. */
struct devssh_keys {
    char     text[DEVSSH_MODULE_BYTES_MAX + 2];
    int      length;
    unsigned count;
};

/* Validate MODULE (BYTES long) and fill OUT. Returns DEVSSH_OK or the first
 * reason it was refused; OUT is empty (length 0) on every refusal. */
int devssh_keys_parse(const void *module, uint64_t bytes, struct devssh_keys *out);

/* A short fixed phrase for a verdict, for the boot log. */
const char *devssh_verdict_name(int verdict);

/* What the provisioner needs to know about a path. exists == 0 means "no such
 * name" and is not an error; the stat operation returns a negative value only
 * for a real failure. */
struct devssh_stat {
    int      exists;
    int      is_directory;
    int      is_regular;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;       /* permission bits only, 0..0777 */
    uint64_t size;
};

/* The file operations provisioning uses, as a table so the same logic runs on
 * the kernel VFS and on the host test's in-memory filesystem. Every function
 * returns 0 (read: a byte count) on success and a negative value on failure. */
struct devssh_fs_ops {
    void *context;
    int (*stat)(void *context, const char *path, struct devssh_stat *out);
    int (*read)(void *context, const char *path, void *buffer, int capacity);
    int (*write)(void *context, const char *path, const void *data, int size);
    int (*make_directory)(void *context, const char *path);
    int (*change_owner)(void *context, const char *path, uint32_t uid, uint32_t gid);
    int (*change_mode)(void *context, const char *path, uint32_t mode);
};

/* What devssh_provision did. */
enum devssh_result {
    DEVSSH_DONE = 0,          /* account, keys and flag are all in place */
    DEVSSH_NO_MODULE,         /* no module on the stick: nothing was touched */
    DEVSSH_MODULE_REFUSED,    /* the validator refused it: nothing was touched */
    DEVSSH_ACCOUNT_CONFLICT,  /* a "dev" row that is not ours: left alone */
    DEVSSH_FS_FAILED          /* a filesystem step failed: sshd.enabled not made */
};

/* Provision the dev account from MODULE. Idempotent: when everything is
 * already in place it performs no mutating operation. Returns a devssh_result.
 * *CHANGES (optional) receives the number of mutating operations performed. */
int devssh_provision(const struct devssh_fs_ops *fs, const void *module,
                     uint64_t module_bytes, unsigned *changes);

/* 1 when STORE (an /etc/passwd image, LENGTH bytes) holds anything other than
 * the locked "dev" row this module writes, or cannot be parsed. 0 when it holds
 * only that row. The window manager locks the desktop behind the greeter when
 * the store has an account to authenticate; the dev row has no password, so
 * locking on it alone would hide the desktop of a board whose keyboard does not
 * work (c/kernel/gui/wm.c, "THE LOCK"). */
int devssh_store_has_other_accounts(const char *store, int length);

#endif /* LOGIT_HWHUD || DEVSSH_HOSTTEST */

#ifdef LOGIT_HWHUD

/* Kernel entry point: find the boot module, provision from it, print one
 * "[devssh]" line per outcome. Call after the root filesystem is mounted and
 * before init (wm_run) starts /bin/login. Safe to call with no module. */
void devssh_provision_boot(void);

/* Replacement for the window manager's "does /etc/passwd exist" lock test in a
 * bring-up build: 1 locks the desktop behind the greeter, 0 starts it. */
int devssh_desktop_locked(void);

#else

/* Not a bring-up build: nothing to do, and no code behind this name. */
static inline void devssh_provision_boot(void)
{
}

#endif

#endif /* LOGIT_DEVSSH_H */
