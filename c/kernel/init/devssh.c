/* Developer SSH channel for bring-up builds: see devssh.h for the whole
 * argument. This file is the validator, the provisioner and the thin adapter
 * that points the provisioner at the kernel VFS.
 *
 * It compiles to NOTHING unless LOGIT_HWHUD (the bring-up kernel) or
 * DEVSSH_HOSTTEST (tests/hw/devssh/devssh_test.c) is defined, so a product
 * kernel carries no symbol from it.
 *
 * Negative controls, selected by -D on the host test only (the kernel build
 * never sets them; tests/hw-devssh.mk proves each turns named checks red):
 *   DEVSSH_NEGCTL_ACCEPT_ANY  the validator accepts any module contents
 *   DEVSSH_NEGCTL_PASSWORD    the account is created WITH a password */
#if defined(LOGIT_HWHUD) || defined(DEVSSH_HOSTTEST)

#include <stddef.h>
#include <stdint.h>
#include "devssh.h"
#include "kprintf.h"
/* Path-qualified like c/kernel/core/settings.c: the kernel gets a second reader
 * of the account store and not a second definition of its format. */
#include "../../apps/coreutils/accounts.h"

/* The only key type the channel accepts, and the shape of its public-key blob
 * (RFC 8709 section 4 with RFC 4253 section 6.6 string encoding):
 *   uint32 11, "ssh-ed25519", uint32 32, 32 key bytes   = 51 bytes. */
#define KEY_TYPE_NAME        "ssh-ed25519"
#define KEY_TYPE_LENGTH      11
#define ED25519_KEY_BYTES    32
#define KEY_BLOB_BYTES       (4 + KEY_TYPE_LENGTH + 4 + ED25519_KEY_BYTES)
/* 51 bytes is a multiple of three, so the base64 text has no '=' padding. */
#define KEY_BASE64_CHARS     68
#define COMMENT_CHARS_MAX    128

#define ROOT_UID  0u
#define ROOT_GID  0u

/* The hash field of the dev row. "!" is the shadow(5) convention for a locked
 * password; here it works because pwhash_check (c/crypto/kdf/pbkdf2.c) returns
 * 0 for any record that does not start with "$pbkdf2-sha256$", for every
 * password including the empty one, and sshd's password method and the console
 * login both go through acct_check_password -> pwhash_check. */
#ifdef DEVSSH_NEGCTL_PASSWORD
/* Negative control: a real record for the password "devssh-negctl" (salt
 * "devssh-negctl-00", 1000 iterations, made with Python hashlib.pbkdf2_hmac).
 * The host test's "no password verifies" checks must go red on this build. */
#define DEVSSH_ACCOUNT_HASH \
    "$pbkdf2-sha256$1000$ZGV2c3NoLW5lZ2N0bC0wMA==$IBRJ7wCldJbE9zxcxqBfD6Prj6RNUrtAep93p/VNJ/M="
#else
#define DEVSSH_ACCOUNT_HASH "!"
#endif

#define FLAG_FILE_TEXT \
    "devssh: sshd enabled by the bring-up kernel for the dev key channel\n"

/* ------------------------------------------------------------------------ *
 *  the key-list validator                                                  *
 * ------------------------------------------------------------------------ */

/* Fixed phrases for the boot log and the host test. */
const char *devssh_verdict_name(int verdict)
{
    switch (verdict) {
    case DEVSSH_OK:         return "ok";
    case DEVSSH_EMPTY:      return "empty";
    case DEVSSH_TOO_BIG:    return "larger than 4096 bytes";
    case DEVSSH_BAD_BYTE:   return "contains a control or non-ASCII byte";
    case DEVSSH_BAD_LINE:   return "a line is not 'ssh-ed25519 <base64> [comment]'";
    case DEVSSH_BAD_TYPE:   return "a key is not ssh-ed25519";
    case DEVSSH_BAD_BASE64: return "a key field is not 68 base64 characters";
    case DEVSSH_BAD_BLOB:   return "a key does not decode to an Ed25519 public key";
    case DEVSSH_NO_KEY:     return "no key line";
    default:                return "unknown";
    }
}

/* Value of one character of the standard base64 alphabet (RFC 4648 section 4),
 * or -1. Why strict: the line is later matched by sshd's own decoder, and a
 * lenient reader here would accept text that decoder rejects. */
static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

/* Decode exactly KEY_BASE64_CHARS characters into the 51-byte blob. Returns 0,
 * or -1 for a bad character. No padding exists at this length, so a '=' is bad. */
static int decode_key_field(const char *field, uint8_t blob[KEY_BLOB_BYTES])
{
    int out = 0;
    for (int group = 0; group < KEY_BASE64_CHARS; group += 4) {
        int a = base64_value(field[group]);
        int b = base64_value(field[group + 1]);
        int c = base64_value(field[group + 2]);
        int d = base64_value(field[group + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0)
            return -1;
        blob[out++] = (uint8_t)((a << 2) | (b >> 4));
        blob[out++] = (uint8_t)((b << 4) | (c >> 2));
        blob[out++] = (uint8_t)((c << 6) | d);
    }
    return 0;
}

/* True when BLOB is the Ed25519 public-key encoding: the type string, then a
 * 32-byte key string. Why: sshd compares the client's blob byte for byte with
 * the decoded line, so a line whose blob is some other key type would sit in
 * authorized_keys and never match anything. */
static int blob_is_ed25519(const uint8_t blob[KEY_BLOB_BYTES])
{
    static const uint8_t prefix[4 + KEY_TYPE_LENGTH] = {
        0, 0, 0, KEY_TYPE_LENGTH,
        's', 's', 'h', '-', 'e', 'd', '2', '5', '5', '1', '9'
    };
    for (int i = 0; i < 4 + KEY_TYPE_LENGTH; i++) {
        if (blob[i] != prefix[i])
            return 0;
    }
    int at = 4 + KEY_TYPE_LENGTH;
    return blob[at] == 0 && blob[at + 1] == 0 && blob[at + 2] == 0 &&
           blob[at + 3] == ED25519_KEY_BYTES;
}

/* Append LENGTH bytes of TEXT to OUT; 0 on success, -1 when it would not fit. */
static int keys_append(struct devssh_keys *out, const char *text, int length)
{
    if (out->length + length > DEVSSH_MODULE_BYTES_MAX + 1)
        return -1;
    for (int i = 0; i < length; i++)
        out->text[out->length++] = text[i];
    out->text[out->length] = 0;
    return 0;
}

/* Validate one line (no newline, no CR) and append its normal form. Returns a
 * devssh_verdict. Why a single space between fields and no options: sshd's
 * matcher (ssh_authkeys_match, c/net/ssh/ssh_auth.c) takes the key type as the
 * first field, so an options prefix could never match, and accepting it here
 * would hide that behind a "valid" file. */
static int parse_key_line(const char *line, int length, struct devssh_keys *out)
{
    int type_end = 0;
    while (type_end < length && line[type_end] != ' ')
        type_end++;
    if (type_end != KEY_TYPE_LENGTH)
        return DEVSSH_BAD_TYPE;
    for (int i = 0; i < KEY_TYPE_LENGTH; i++) {
        if (line[i] != KEY_TYPE_NAME[i])
            return DEVSSH_BAD_TYPE;
    }
    if (type_end == length)
        return DEVSSH_BAD_LINE;

    const char *field = line + type_end + 1;
    int field_room = length - type_end - 1;
    int field_length = 0;
    while (field_length < field_room && field[field_length] != ' ')
        field_length++;
    if (field_length == 0)
        return DEVSSH_BAD_LINE;
    if (field_length != KEY_BASE64_CHARS)
        return DEVSSH_BAD_BASE64;
    uint8_t blob[KEY_BLOB_BYTES];
    if (decode_key_field(field, blob) != 0)
        return DEVSSH_BAD_BASE64;
    if (!blob_is_ed25519(blob))
        return DEVSSH_BAD_BLOB;

    const char *comment = field + field_length;
    int comment_length = field_room - field_length;
    while (comment_length > 0 && *comment == ' ') {
        comment++;
        comment_length--;
    }
    while (comment_length > 0 && comment[comment_length - 1] == ' ')
        comment_length--;
    if (comment_length > COMMENT_CHARS_MAX)
        return DEVSSH_BAD_LINE;

    if (keys_append(out, KEY_TYPE_NAME " ", KEY_TYPE_LENGTH + 1) != 0 ||
        keys_append(out, field, field_length) != 0)
        return DEVSSH_TOO_BIG;
    if (comment_length > 0) {
        if (keys_append(out, " ", 1) != 0 || keys_append(out, comment, comment_length) != 0)
            return DEVSSH_TOO_BIG;
    }
    if (keys_append(out, "\n", 1) != 0)
        return DEVSSH_TOO_BIG;
    out->count++;
    return DEVSSH_OK;
}

/* The module -> authorized_keys text. See devssh.h for the contract. Why every
 * byte is screened before any line is read: the module is firmware-delivered
 * data from a removable stick, and a NUL or high byte could end the text early
 * for one reader and not for another. */
int devssh_keys_parse(const void *module, uint64_t bytes, struct devssh_keys *out)
{
    out->length = 0;
    out->count = 0;
    out->text[0] = 0;
#ifdef DEVSSH_NEGCTL_ACCEPT_ANY
    /* Negative control: believe whatever the stick holds, truncated to fit.
     * The line parser stays referenced so this build is -Werror clean. */
    (void)parse_key_line;
    if (!module || bytes == 0)
        return DEVSSH_EMPTY;
    const char *raw = module;
    int kept = bytes > DEVSSH_MODULE_BYTES_MAX ? DEVSSH_MODULE_BYTES_MAX : (int)bytes;
    keys_append(out, raw, kept);
    out->count = 1;
    return DEVSSH_OK;
#else
    if (!module || bytes == 0)
        return DEVSSH_EMPTY;
    if (bytes > DEVSSH_MODULE_BYTES_MAX)
        return DEVSSH_TOO_BIG;
    const char *text = module;
    int length = (int)bytes;

    for (int i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n')
            continue;
        if (c == '\r') {
            if (i + 1 < length && text[i + 1] != '\n')
                return DEVSSH_BAD_BYTE;
            continue;
        }
        if (c < 0x20 || c > 0x7e)
            return DEVSSH_BAD_BYTE;
    }

    int verdict = DEVSSH_OK;
    int position = 0;
    while (position < length && verdict == DEVSSH_OK) {
        int start = position;
        while (position < length && text[position] != '\n')
            position++;
        int end = position;
        if (position < length)
            position++;
        if (end > start && text[end - 1] == '\r')
            end--;
        if (end == start)
            continue;
        verdict = parse_key_line(text + start, end - start, out);
    }
    if (verdict == DEVSSH_OK && out->count == 0)
        verdict = DEVSSH_NO_KEY;
    if (verdict != DEVSSH_OK) {
        out->length = 0;
        out->count = 0;
        out->text[0] = 0;
    }
    return verdict;
#endif
}

/* ------------------------------------------------------------------------ *
 *  the account store                                                       *
 * ------------------------------------------------------------------------ */

/* Compare two NUL-terminated strings for equality. */
static int same_text(const char *a, const char *b)
{
    return acct_eq(a, b);
}

/* 1 when STORE holds anything but the locked dev row; see devssh.h. Why a
 * garbage line counts as "other": the product rule is "the file exists, so
 * lock"; this relaxes it only for a store this module can fully account for. */
int devssh_store_has_other_accounts(const char *store, int length)
{
    int position = 0;
    int dev_rows = 0;
    while (position < length) {
        int start = position;
        while (position < length && store[position] != '\n')
            position++;
        int end = position;
        if (position < length)
            position++;
        int first = start;
        while (first < end && (store[first] == ' ' || store[first] == '\t'))
            first++;
        if (first == end || store[first] == '#')
            continue;
        struct account row;
        if (!acct_parse_line(store + start, end - start, &row))
            return 1;
        if (!same_text(row.name, DEVSSH_ACCOUNT_NAME) ||
            !same_text(row.hash, DEVSSH_ACCOUNT_HASH))
            return 1;
        dev_rows++;
    }
    return dev_rows == 0;
}

/* ------------------------------------------------------------------------ *
 *  provisioning                                                            *
 * ------------------------------------------------------------------------ */

/* Scratch for one provisioning run. Static because the run happens once, early,
 * on one CPU, and 12 KiB is too much to put on a boot stack. */
static struct devssh_keys wanted_keys;
static char account_store[ACCT_MAX + 2];
static char compare_buffer[DEVSSH_MODULE_BYTES_MAX + 2];

/* Count one mutating operation. */
static void count_change(unsigned *changes)
{
    if (changes)
        (*changes)++;
}

/* Make sure DIRECTORY exists as a directory. A new one is chown'd and chmod'd
 * to OWNER_UID/OWNER_GID/MODE. An existing one is left alone unless ADOPT is
 * set, in which case its owner and mode are converged. Returns 0 or -1. Why two
 * modes: /etc and /home belong to the image and must not change, while
 * /home/dev and /home/dev/.ssh are this account's and sshd's key lookup depends
 * on their being private. */
static int ensure_directory(const struct devssh_fs_ops *fs, const char *directory,
                            uint32_t owner_uid, uint32_t owner_gid, uint32_t mode,
                            int adopt, unsigned *changes)
{
    struct devssh_stat info;
    if (fs->stat(fs->context, directory, &info) < 0)
        return -1;
    int created = 0;
    if (!info.exists) {
        if (fs->make_directory(fs->context, directory) < 0)
            return -1;
        count_change(changes);
        created = 1;
        if (fs->stat(fs->context, directory, &info) < 0 || !info.exists)
            return -1;
    }
    if (!info.is_directory)
        return -1;
    if (!created && !adopt)
        return 0;
    if (info.uid != owner_uid || info.gid != owner_gid) {
        if (fs->change_owner(fs->context, directory, owner_uid, owner_gid) < 0)
            return -1;
        count_change(changes);
    }
    if ((info.mode & 0777) != mode) {
        if (fs->change_mode(fs->context, directory, mode) < 0)
            return -1;
        count_change(changes);
    }
    return 0;
}

/* Make PATH a regular file holding exactly DATA (SIZE bytes) with the given
 * owner and mode, performing only the operations that are needed. Returns 0 or
 * -1. Why the content is compared first: a second boot with the same stick must
 * not rewrite the root partition, and a changed stick must replace the keys. */
static int ensure_file(const struct devssh_fs_ops *fs, const char *path,
                       const char *data, int size, uint32_t owner_uid,
                       uint32_t owner_gid, uint32_t mode, unsigned *changes)
{
    struct devssh_stat info;
    if (fs->stat(fs->context, path, &info) < 0)
        return -1;
    if (info.exists && !info.is_regular)
        return -1;
    int content_matches = 0;
    if (info.exists && info.size == (uint64_t)size && size <= DEVSSH_MODULE_BYTES_MAX + 1) {
        int got = fs->read(fs->context, path, compare_buffer, DEVSSH_MODULE_BYTES_MAX + 1);
        content_matches = got == size;
        for (int i = 0; content_matches && i < size; i++) {
            if (compare_buffer[i] != data[i])
                content_matches = 0;
        }
    }
    if (!content_matches) {
        if (fs->write(fs->context, path, data, size) < 0)
            return -1;
        count_change(changes);
        if (fs->stat(fs->context, path, &info) < 0 || !info.exists || !info.is_regular)
            return -1;
    }
    if (info.uid != owner_uid || info.gid != owner_gid) {
        if (fs->change_owner(fs->context, path, owner_uid, owner_gid) < 0)
            return -1;
        count_change(changes);
    }
    if ((info.mode & 0777) != mode) {
        if (fs->change_mode(fs->context, path, mode) < 0)
            return -1;
        count_change(changes);
    }
    return 0;
}

/* Make /etc/sshd.enabled satisfy boot_services.h boot_service(): a regular
 * file, uid 0, no group or world write bit. An existing file that already does
 * is left exactly as it is (its content never matters to the check). Returns 0
 * or -1. */
static int ensure_enable_flag(const struct devssh_fs_ops *fs, unsigned *changes)
{
    struct devssh_stat info;
    if (fs->stat(fs->context, DEVSSH_ENABLE_FLAG_PATH, &info) < 0)
        return -1;
    if (info.exists && !info.is_regular)
        return -1;
    if (info.exists && info.uid == ROOT_UID && (info.mode & 022) == 0)
        return 0;
    if (!info.exists) {
        if (fs->write(fs->context, DEVSSH_ENABLE_FLAG_PATH, FLAG_FILE_TEXT,
                      (int)sizeof(FLAG_FILE_TEXT) - 1) < 0)
            return -1;
        count_change(changes);
        if (fs->stat(fs->context, DEVSSH_ENABLE_FLAG_PATH, &info) < 0 || !info.exists)
            return -1;
    }
    if (info.uid != ROOT_UID || info.gid != ROOT_GID) {
        if (fs->change_owner(fs->context, DEVSSH_ENABLE_FLAG_PATH, ROOT_UID, ROOT_GID) < 0)
            return -1;
        count_change(changes);
    }
    if ((info.mode & 022) != 0 || (info.mode & 0777) != 0644) {
        if (fs->change_mode(fs->context, DEVSSH_ENABLE_FLAG_PATH, 0644) < 0)
            return -1;
        count_change(changes);
    }
    return 0;
}

/* Find or create the dev row in the store. On success fills ROW with the row
 * sshd will read and returns 0; returns DEVSSH_ACCOUNT_CONFLICT for a "dev" row
 * that is not this module's, or DEVSSH_FS_FAILED. Why the conflict rule: an
 * owner who enrolled a real account named dev must not have this module's key
 * appended to their home. */
static int ensure_account(const struct devssh_fs_ops *fs, struct account *row,
                          unsigned *changes)
{
    struct devssh_stat info;
    if (fs->stat(fs->context, DEVSSH_STORE_PATH, &info) < 0)
        return DEVSSH_FS_FAILED;
    int length = 0;
    if (info.exists) {
        if (!info.is_regular || info.size > ACCT_MAX)
            return DEVSSH_FS_FAILED;
        length = fs->read(fs->context, DEVSSH_STORE_PATH, account_store, ACCT_MAX);
        if (length < 0)
            return DEVSSH_FS_FAILED;
    }
    account_store[length] = 0;

    if (acct_find(account_store, length, DEVSSH_ACCOUNT_NAME, row)) {
        if (!same_text(row->hash, DEVSSH_ACCOUNT_HASH) ||
            !same_text(row->home, DEVSSH_HOME_PATH) ||
            !same_text(row->shell, "/bin/sh") ||
            row->uid < ACCT_FIRST_UID)
            return DEVSSH_ACCOUNT_CONFLICT;
        return 0;
    }

    unsigned next_uid = acct_max_uid(account_store, length) + 1u;
    if (next_uid < ACCT_FIRST_UID)
        next_uid = ACCT_FIRST_UID;
    acct_cpyn(row->name, DEVSSH_ACCOUNT_NAME, acct_len(DEVSSH_ACCOUNT_NAME), ACCT_NAME);
    acct_cpyn(row->hash, DEVSSH_ACCOUNT_HASH, acct_len(DEVSSH_ACCOUNT_HASH), ACCT_HASH);
    row->uid = next_uid;
    row->gid = next_uid;
    acct_cpyn(row->home, DEVSSH_HOME_PATH, acct_len(DEVSSH_HOME_PATH), ACCT_PATH);
    acct_cpyn(row->shell, "/bin/sh", 7, ACCT_PATH);
    int new_length = acct_append(account_store, length, ACCT_MAX, row);
    if (new_length < 0)
        return DEVSSH_FS_FAILED;
    /* The store is the secret-bearing file of the machine: root:root 0600, the
     * same as /bin/login's enrolment makes it (login.c enrol()). */
    if (ensure_file(fs, DEVSSH_STORE_PATH, account_store, new_length,
                    ROOT_UID, ROOT_GID, 0600, changes) != 0)
        return DEVSSH_FS_FAILED;
    return 0;
}

/* See devssh.h. The order is the safety argument: the account, then its
 * private directories, then the keys, and only then the flag that makes
 * boot_services() start sshd, so a failure part way never leaves sshd running
 * with no keys to check. */
int devssh_provision(const struct devssh_fs_ops *fs, const void *module,
                     uint64_t module_bytes, unsigned *changes)
{
    if (changes)
        *changes = 0;
    if (!module)
        return DEVSSH_NO_MODULE;
    int verdict = devssh_keys_parse(module, module_bytes, &wanted_keys);
    if (verdict != DEVSSH_OK) {
        kprintf("[devssh] module refused: %s; nothing changed\n", devssh_verdict_name(verdict));
        return DEVSSH_MODULE_REFUSED;
    }

    /* /etc and /home first: the store write below and the home directory after
     * it both need their parent to exist, and an image that lacks either (the
     * host test's empty filesystem) must not fail half way. Existing ones are
     * never altered. */
    if (ensure_directory(fs, "/etc", ROOT_UID, ROOT_GID, 0755, 0, changes) != 0 ||
        ensure_directory(fs, "/home", ROOT_UID, ROOT_GID, 0755, 0, changes) != 0) {
        kprintf("[devssh] cannot prepare /etc or /home; sshd not enabled\n");
        return DEVSSH_FS_FAILED;
    }

    struct account row;
    int account_result = ensure_account(fs, &row, changes);
    if (account_result == DEVSSH_ACCOUNT_CONFLICT) {
        kprintf("[devssh] an account named %s exists that this module did not make; left alone\n",
                DEVSSH_ACCOUNT_NAME);
        return DEVSSH_ACCOUNT_CONFLICT;
    }
    if (account_result != 0) {
        kprintf("[devssh] cannot read or write %s; sshd not enabled\n", DEVSSH_STORE_PATH);
        return DEVSSH_FS_FAILED;
    }

    if (ensure_directory(fs, DEVSSH_HOME_PATH, row.uid, row.gid, 0700, 1, changes) != 0 ||
        ensure_directory(fs, DEVSSH_SSH_DIR_PATH, row.uid, row.gid, 0700, 1, changes) != 0) {
        kprintf("[devssh] cannot prepare %s; sshd not enabled\n", DEVSSH_HOME_PATH);
        return DEVSSH_FS_FAILED;
    }
    if (ensure_file(fs, DEVSSH_AUTHKEYS_PATH, wanted_keys.text, wanted_keys.length,
                    row.uid, row.gid, 0600, changes) != 0) {
        kprintf("[devssh] cannot write %s; sshd not enabled\n", DEVSSH_AUTHKEYS_PATH);
        return DEVSSH_FS_FAILED;
    }
    if (ensure_enable_flag(fs, changes) != 0) {
        kprintf("[devssh] cannot create %s\n", DEVSSH_ENABLE_FLAG_PATH);
        return DEVSSH_FS_FAILED;
    }
    return DEVSSH_DONE;
}

/* ------------------------------------------------------------------------ *
 *  the kernel adapter                                                      *
 * ------------------------------------------------------------------------ */

#if defined(LOGIT_HWHUD) && !defined(DEVSSH_HOSTTEST)

#include "bootmod.h"
#include "vfs.h"

/* stat for the provisioner: lstat semantics (a symlink is reported as itself,
 * never followed, because sshd lstat()s authorized_keys too). A missing name,
 * or a missing parent directory, is "does not exist"; anything else negative
 * is a failure. */
static int kernel_stat(void *context, const char *path, struct devssh_stat *out)
{
    (void)context;
    struct vattr attr;
    out->exists = 0;
    out->is_directory = 0;
    out->is_regular = 0;
    out->uid = 0;
    out->gid = 0;
    out->mode = 0;
    out->size = 0;
    int rc = vfs_statx(path, &attr, 0);
    if (rc == VFS_ENOENT)
        return 0;
    if (rc < 0)
        return rc;
    out->exists = 1;
    out->is_directory = attr.type == VT_DIR;
    out->is_regular = attr.type == VT_REG;
    out->uid = attr.uid;
    out->gid = attr.gid;
    out->mode = attr.mode & 0777;
    out->size = attr.size;
    return 0;
}

/* Read a whole small file; fails when it would not fit CAPACITY. */
static int kernel_read(void *context, const char *path, void *buffer, int capacity)
{
    (void)context;
    int size = vfs_size(path);
    if (size < 0)
        return size;
    if (size > capacity)
        return -1;
    return vfs_read(path, buffer, capacity);
}

/* Write a whole file (create or replace); 0 only when every byte was taken. */
static int kernel_write(void *context, const char *path, const void *data, int size)
{
    (void)context;
    int wrote = vfs_write(path, data, size);
    return wrote == size ? 0 : -1;
}

/* mkdir for the provisioner; the VFS stamps the new directory with the caller's
 * credential, which is root here. */
static int kernel_make_directory(void *context, const char *path)
{
    (void)context;
    return vfs_mkdir(path) < 0 ? -1 : 0;
}

/* chown for the provisioner; vfs_chown is root-only, and the kernel is root. */
static int kernel_change_owner(void *context, const char *path, uint32_t uid, uint32_t gid)
{
    (void)context;
    return vfs_chown(path, uid, gid) < 0 ? -1 : 0;
}

/* chmod for the provisioner; the mode reaches the medium (LogitFS setattr), so
 * 0600 on the account store survives a power cut. */
static int kernel_change_mode(void *context, const char *path, uint32_t mode)
{
    (void)context;
    return vfs_chmod(path, mode) < 0 ? -1 : 0;
}

static const struct devssh_fs_ops kernel_fs = {
    0,
    kernel_stat,
    kernel_read,
    kernel_write,
    kernel_make_directory,
    kernel_change_owner,
    kernel_change_mode,
};

/* The boot hook; see devssh.h. The kernel is root here (no current process),
 * which is what lets vfs_chown succeed (vfs.c vfs_chown is root-only). */
void devssh_provision_boot(void)
{
    uint64_t bytes = 0;
    const void *module = bootmod_find(DEVSSH_MODULE_NAME, &bytes);
    if (!module) {
        kprintf("[devssh] no %s module on the boot volume; bring-up ssh unchanged\n",
                DEVSSH_MODULE_NAME);
        return;
    }
    unsigned changes = 0;
    int result = devssh_provision(&kernel_fs, module, bytes, &changes);
    if (result == DEVSSH_DONE) {
        kprintf("[devssh] ready: account %s (key-only), %u change(s); sshd starts at login\n",
                DEVSSH_ACCOUNT_NAME, changes);
    }
}

/* The desktop-lock decision for a bring-up build; see devssh.h. Fails safe:
 * a store that is too large or unreadable still locks. */
int devssh_desktop_locked(void)
{
    int size = vfs_size(DEVSSH_STORE_PATH);
    if (size <= 0)
        return 0;
    if (size > ACCT_MAX)
        return 1;
    int length = vfs_read(DEVSSH_STORE_PATH, account_store, size);
    if (length <= 0)
        return 1;
    account_store[length] = 0;
    int locked = devssh_store_has_other_accounts(account_store, length);
    /* The hashes were read with root's right to; do not leave them in a static. */
    for (int i = 0; i <= length; i++)
        account_store[i] = 0;
    return locked;
}

#endif /* LOGIT_HWHUD && !DEVSSH_HOSTTEST */

#endif /* LOGIT_HWHUD || DEVSSH_HOSTTEST */

/* A translation unit must declare something even when every guard above is off
 * (a product build); this typedef emits no symbol. */
typedef int devssh_empty_unit_placeholder;
