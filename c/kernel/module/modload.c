/* The kernel half of the module loader: memory, credentials, the driver
 * registry, the lifecycle, and the syscall.
 *
 * modelf.c does the format and the arithmetic and is pure. Everything that
 * needs a kernel underneath it is here, which is also everything the host test
 * cannot reach -- so this file is gated on device (make test-module) and that
 * one is gated on the host (make test-modreloc).
 *
 * LOCKING: none of its own. Every entry point here is reached from
 * syscall_dispatch, which runs under the BKL (mod_syscall is not on
 * syscall_is_bkl_free's allow-list and must not be added to it: it calls
 * kmalloc, the VFS and driver_register, and the last of those mutates a global
 * list with no lock of its own because it was written for boot-time use). The
 * one place that is NOT a syscall is mod_dump(), which reads the table and is
 * safe to call from a debugger stop.
 */

#include <stdint.h>
#include <stddef.h>
#include "module.h"
#include "driver.h"
#include "kheap.h"
#include "kprintf.h"
#include "vfs.h"
#include "vfs_cred.h"
#include "usercopy.h"
#include "logit_abi.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

static struct kmodule g_mods[MOD_MAX];
static int            g_nmod;          /* slots ever used; slots are not reused */
static int            g_next_id = 1;

int mod_count(void) { return g_nmod; }

const struct kmodule *mod_at(int i)
{
    return (i >= 0 && i < g_nmod) ? &g_mods[i] : NULL;
}

/* ------------------------------------------------------------- the name -- */
/* Basename with the extension dropped: "/lib/modules/edu.ko" -> "edu". The
 * name is what mod_dump prints and what the duplicate check compares, so it
 * has to be derived from the path rather than from anything inside the object
 * -- an ET_REL file has no name of its own (the FILE symbol is the SOURCE
 * file, which is not the same thing and is absent when built from stdin). */
static void modname(char *dst, int max, const char *path)
{
    /* Zero the WHOLE buffer, not just up to the NUL. struct kmodule::name is
     * copied wholesale into a `struct logit_modinfo` and handed to ring 3 by
     * SYS_MODULE_LIST, so the bytes after the terminator are not padding --
     * they are whatever was on the kernel stack, delivered to an unprivileged
     * caller. A NUL-terminating loop alone reads correctly and leaks. */
    for (int i = 0; i < max; i++) dst[i] = 0;
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    int n = 0;
    for (const char *p = b; *p && n < max - 1; p++) {
        if (*p == '.') break;
        dst[n++] = *p;
    }
    if (n == 0) { dst[n++] = '?'; }
    dst[n] = 0;
}

static struct kmodule *find_by_name(const char *name)
{
    for (int i = 0; i < g_nmod; i++) {
        if (!g_mods[i].id) continue;
        const char *a = g_mods[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return &g_mods[i];
    }
    return NULL;
}

/* ------------------------------------------------------------- resolving -- */
static void *resolve_ksym(const char *name, void *ctx)
{
    (void)ctx;
    return ksym_lookup(name);
}

/* --------------------------------------------------------------- loading -- */
int mod_load(const char *path)
{
    if (!path || !path[0]) return MOD_E_INVAL;

    char name[MOD_NAME_LEN];
    modname(name, MOD_NAME_LEN, path);
    /* Refused BEFORE the file is read. Loading the same driver twice would
     * register two struct drivers matching the same device; the second never
     * binds (a device is bound at most once) so it would look like it worked
     * and leave a permanent, invisible duplicate in the probe list. */
    if (find_by_name(name)) return MOD_E_DUP;

    /* The slot is CHOSEN here and CLAIMED at the bottom, after the module is
     * relocated and cannot fail any more. Claiming it up front (g_nmod++) and
     * returning early on a bad file leaves a permanently occupied entry that
     * mod_count() reports and mod_dump() cannot print -- a table that grows
     * with every typo. */
    int slot = -1;
    for (int i = 0; i < g_nmod; i++) if (!g_mods[i].id) { slot = i; break; }
    if (slot < 0 && g_nmod >= MOD_MAX) return MOD_E_FULL;

    int fsz = vfs_size(path);
    if (fsz <= 0) return MOD_E_NOFILE;
    if ((uint32_t)fsz > MOD_MAX_IMAGE) return MOD_E_TOOBIG;

    /* The file is read whole. LogitFS reads a whole file per call anyway
     * (vfs_read takes no offset for this path) and an ET_REL object has no
     * usable streaming order -- the section table is at the END, so the first
     * thing the parser needs is the last thing a stream delivers. */
    uint8_t *img = kmalloc((size_t)fsz);
    if (!img) return MOD_E_NOMEM;
    int got = vfs_read(path, img, fsz);
    if (got != fsz) { kfree(img); return MOD_E_NOFILE; }

    long need = mod_elf_size(img, (uint32_t)fsz);
    if (need < 0) { kfree(img); return (int)need; }

    uint8_t *blk = kmalloc((size_t)need);
    if (!blk) { kfree(img); return MOD_E_NOMEM; }
    memset(blk, 0, (size_t)need);

    struct mod_layout lay;
    const char *undef = NULL;
    int e = mod_elf_load(img, (uint32_t)fsz, blk, (uint32_t)need,
                         resolve_ksym, NULL, &lay, &undef);
    kfree(img);                       /* relocated; the file bytes are dead */
    if (e < 0) {
        /* Named, always. "MOD_E_UNDEF" alone sends the reader to diff two nm
         * outputs by hand; the symbol name sends them to ksyms.c, which is
         * where the fix is. */
        if (e == MOD_E_UNDEF)
            kprintf("[mod] %s: undefined symbol '%s' (not exported -- see "
                    "c/kernel/module/ksyms.c)\n", name, undef ? undef : "?");
        else
            kprintf("[mod] %s: load failed, err %d\n", name, e);
        kfree(blk);
        return e;
    }

    if (slot < 0) slot = g_nmod++;              /* claimed: nothing can fail now */
    struct kmodule *m = &g_mods[slot];
    memset(m, 0, sizeof *m);
    m->id = g_next_id++;
    memcpy(m->name, name, MOD_NAME_LEN);
    m->base = blk;
    m->size = (uint32_t)need;
    m->text_off = lay.text_off;
    m->text_size = lay.text_size;

    /* THE REGISTRATION HALF, and this is the whole of it.
     *
     * DRIVER_DECLARE put a `struct driver *` in the object's own
     * `logit_drivers` section. The linker put nothing there for us -- the
     * kernel's __start_/__stop_logit_drivers bracket only the drivers that
     * were compiled IN. So the loader walks the module's copy of that section
     * and calls the same driver_register() the static path calls, which means
     * a module's driver and a built-in driver are the same kind of thing to
     * the device model from that point on. No second registry, no "module
     * driver" flag, no branch in dev_probe_all.
     *
     * THE COST, said out loud: registration order. Static drivers are already
     * in the list, so a module's driver is tried LAST, and binding is
     * first-driver-wins. A module cannot therefore override a built-in driver
     * for the same device -- which is exactly what a quirk driver would want
     * to do, and is the first thing this design cannot express. Fixing it
     * needs driver_register_first(), one function in device.c. */
    struct driver **ds = (struct driver **)lay.drv_start;
    struct driver **de = (struct driver **)lay.drv_stop;
    for (struct driver **p = ds; p && p < de; p++) {
        if (!*p) continue;
        driver_register(*p);
        m->ndrivers++;
    }

    int bound = dev_probe_all();

    /* THE REFCOUNT, computed rather than maintained. A module is "in use" if
     * some device in the registry is bound to one of its drivers, and the
     * device registry already knows that -- so counting it here reads the
     * truth instead of keeping a second copy that can drift. The whole point
     * of the rmap invariant in c/kernel/mm is the same idea: two independently
     * maintained numbers that must agree, versus one number read where it
     * lives. Here there is only one place it lives. */
    m->nbound = 0;
    for (int i = 0; i < dev_count(); i++) {
        struct device *d = dev_at(i);
        if (!d || !d->drv) continue;
        for (struct driver **p = ds; p && p < de; p++)
            if (*p == d->drv) { m->nbound++; break; }
    }

    kprintf("[mod] %s: loaded at %x, %d bytes, %d driver(s), %d bound "
            "(probe pass bound %d)\n",
            m->name, (unsigned)(uintptr_t)m->base, (int)m->size,
            m->ndrivers, m->nbound, bound);
    return m->id;
}

/* --------------------------------------------------------------- unload --
 * REFUSED, and the refusal is the honest implementation rather than the
 * missing one. This is the house rule from c/apps/libc (flock returns ENOSYS
 * because a caller that gets 0 believes it holds a lock) applied to a case
 * where the consequence is worse than a wrong belief.
 *
 * To unload a module three things must happen and only two of them can:
 *   1. its drivers must leave the device model's `g_drivers` list. THEY
 *      CANNOT. device.c has driver_register() and no driver_unregister(); the
 *      list is a bare singly-linked chain of pointers, written for boot-time
 *      use, and nothing removes from it.
 *   2. any device it bound must be unbound. dev_unbind() exists.
 *   3. its block must be kfree'd. Trivial.
 *
 * Doing 2 and 3 without 1 leaves a `struct driver *` in a live global list
 * pointing into freed kernel heap, whose ->name, ->match and ->probe are read
 * by the very next dev_probe_all() -- i.e. a use-after-free in ring 0 with the
 * BKL held, reached by any later module load. That is strictly worse than not
 * being able to unload, and it is the failure a "best effort" unload here
 * would have shipped.
 *
 * WHAT THE DEVICE MODEL NEEDS TO EXPOSE, precisely, for this to become real:
 *
 *     void driver_unregister(struct driver *drv);   // unlink from g_drivers,
 *                                                   // fix g_drivers_tail,
 *                                                   // set drv->next = NULL
 *
 * with dev_unbind() called for each of its devices first. That is one function
 * in c/drivers/core/device.c, a file this line of work does not own. Given it,
 * the rest of unload is about fifteen lines here and the refcount above
 * (m->nbound) is already the gate that says whether it is allowed. */
int mod_unload(int id)
{
    (void)id;
    return MOD_E_NOUNLOAD;
}

void mod_dump(void)
{
    kprintf("[mod] %d loaded, %d exported symbols\n", g_nmod, ksym_count());
    for (int i = 0; i < g_nmod; i++) {
        if (!g_mods[i].id) continue;
        kprintf("  #%d %s base=%x size=%d text=+%x/%d drivers=%d bound=%d\n",
                g_mods[i].id, g_mods[i].name,
                (unsigned)(uintptr_t)g_mods[i].base, (int)g_mods[i].size,
                (unsigned)g_mods[i].text_off, (int)g_mods[i].text_size,
                g_mods[i].ndrivers, g_mods[i].nbound);
    }
}

/* -------------------------------------------------------------- syscall -- */
/* uid 0, the same check and the same idiom SYS_POWEROFF uses. See module.h for
 * the full argument; the short form is that this is a real state on this
 * machine (/bin/login sets it, vfs_meta persists modes across a reboot) rather
 * than a decoration, and that the read of the module file itself goes through
 * the VFS as the caller, so the file's own mode applies on top. */
static int is_root(void)
{
    struct vcred me;
    vfs_cred_current(&me);
    return me.uid == 0;
}

long mod_syscall(long n, long a0, long a1, long a2)
{
    (void)a2;
    switch (n) {

    case SYS_MODULE_LOAD: {
        if (!is_root()) return MOD_E_PERM;
        char path[192];
        if (user_copy_string(path, sizeof path, (const char *)a0) < 0)
            return MOD_E_INVAL;
        return mod_load(path);
    }

    case SYS_MODULE_UNLOAD:
        if (!is_root()) return MOD_E_PERM;
        return mod_unload((int)a0);

    case SYS_MODULE_LIST: {
        /* NOT root-only. Which modules are loaded is diagnostic, the same
         * class of fact as dev_dump()'s device list, and an unprivileged
         * process that cannot see them debugs blind. The addresses are the
         * part worth withholding and are not in this struct. */
        struct logit_modinfo *out = (struct logit_modinfo *)a0;
        int max = (int)a1;
        if (max < 0) return MOD_E_INVAL;
        if (max > 0 && !out) return MOD_E_INVAL;
        /* (NULL, 0) dumps to the console instead of copying out, which is how
         * SYS_MEMINFO already spells "tell the kernel to report" (mmsys.c's
         * MMCTL_*). It also keeps mod_dump() reachable: an inspection function
         * with no caller is the one that has quietly stopped compiling by the
         * time somebody needs it at three in the morning. */
        if (!out && max == 0) mod_dump();
        int n_out = 0;
        for (int i = 0; i < g_nmod && n_out < max; i++) {
            if (!g_mods[i].id) continue;
            struct logit_modinfo mi;
            memset(&mi, 0, sizeof mi);
            mi.id = g_mods[i].id;
            memcpy(mi.name, g_mods[i].name, MOD_NAME_LEN);
            mi.size = g_mods[i].size;
            mi.ndrivers = g_mods[i].ndrivers;
            mi.nbound = g_mods[i].nbound;
            if (user_copy_to(&out[n_out], &mi, sizeof mi) < 0) return MOD_E_INVAL;
            n_out++;
        }
        /* The COUNT of loaded modules, which may exceed `max`: a caller sizing
         * a buffer needs to know it was short. Returning n_out would make a
         * truncated answer indistinguishable from a complete one. */
        int total = 0;
        for (int i = 0; i < g_nmod; i++) if (g_mods[i].id) total++;
        return total;
    }

    case SYS_MODULE_SYM: {
        /* Root-only, and the reasoning is the reverse of the usual one: root
         * can load a module that prints any kernel address it likes, so
         * withholding one from root protects nothing. The check is here for
         * the NON-root caller, for whom a kernel text address is the one piece
         * of information this whole subsystem should not hand out. */
        if (!is_root()) return MOD_E_PERM;
        char nm[64];
        if (user_copy_string(nm, sizeof nm, (const char *)a0) < 0)
            return MOD_E_INVAL;
        void *a = ksym_lookup(nm);
        if (!a) return 0;                 /* 0 = not exported. See below. */
        if (a1) {
            uint64_t v = (uint64_t)(uintptr_t)a;
            if (user_copy_to((void *)a1, &v, sizeof v) < 0) return MOD_E_INVAL;
        }
        /* 1, not the address: a syscall returning a pointer as its status
         * cannot express "not found" without picking an address that means it,
         * and every such choice is wrong on some machine. The address goes to
         * the out-parameter, which is optional -- pass NULL to ask only
         * whether the name is part of the module ABI. */
        return 1;
    }

    default:
        return MOD_E_INVAL;
    }
}
