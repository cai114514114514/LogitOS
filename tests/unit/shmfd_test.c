/* Host control for the actual per-process SHM handle table. Segment references
 * are modelled here so an unauthorized MAP/CLOSE changes a visible counter.
 * The negative build uses shmfd.c's old global-slot interpretation and must
 * print the two specific failures, not merely exit nonzero for another reason. */
#include <stdio.h>
#include <string.h>
#include "proc.h"
#include "shm.h"

static int refs[SHM_SEGMAX];
static int checks, fails;
static int locks;

#define CHECK(ok, message) do { \
    checks++; \
    if (!(ok)) { fails++; printf("FAIL %s\n", message); } \
} while (0)

uint64_t spin_lock_irqsave(spinlock_t *lock) { (void)lock; locks++; return 0; }
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
{ (void)lock; (void)flags; }
int shm_ref(int sh)
{
    if (sh < 0 || sh >= SHM_SEGMAX || refs[sh] <= 0) return -1;
    refs[sh]++;
    return 0;
}
void shm_put(int sh)
{
    if (sh >= 0 && sh < SHM_SEGMAX && refs[sh] > 0) refs[sh]--;
}

static void init_proc(struct proc *p)
{
    memset(p, 0, sizeof *p);
    proc_shm_reset(p);
}

int main(void)
{
    struct proc owner, foreign, child;
    init_proc(&owner);
    init_proc(&foreign);
    init_proc(&child);
    proc_shm_close_all(&foreign);
    CHECK(locks == 0, "empty process exit takes no SHM table locks");

    refs[0] = 1;                       /* creator's segment reference */
    int h = proc_shm_install(&owner, 0, 1);
    CHECK(h == 0, "owner receives its first local handle");
    CHECK(proc_shm_acquire(&foreign, h, 0, NULL) < 0,
          "foreign process cannot map guessed segment slot");
    CHECK(proc_shm_close(&foreign, h) < 0,
          "foreign process cannot close guessed segment slot");
    CHECK(refs[0] == 1, "foreign calls leave the owner reference intact");
    if (fails) return 1;              /* the control stops at the claimed bug */

    refs[2] = 1;
    int foreign_h = proc_shm_install(&foreign, 2, 0);
    CHECK(foreign_h == h, "different processes may hold the same local number");
    int writable = -1;
    int sh = proc_shm_acquire(&foreign, foreign_h, 0, &writable);
    CHECK(sh == 2, "equal local numbers resolve to each process's own segment");
    CHECK(writable == 0, "read-only open carries its immutable write ceiling");
    shm_put(sh);
    CHECK(proc_shm_close(&foreign, foreign_h) == 0 && refs[2] == 0,
          "foreign close releases only its own segment");

    refs[1] = 1;
    int read_h = proc_shm_install(&owner, 1, 0);
    CHECK(read_h >= 0, "read-only open receives a separate handle");
    CHECK(proc_shm_acquire(&owner, read_h, 1, NULL) < 0,
          "read-only handle cannot make a writable mapping");
    sh = proc_shm_acquire(&owner, read_h, 0, &writable);
    CHECK(sh == 1 && refs[1] == 2, "read mapping pins a transient reference");
    CHECK(proc_shm_close(&owner, read_h) == 0 && refs[1] == 1,
          "close leaves an in-progress map's transient reference alive");
    shm_put(sh);
    CHECK(refs[1] == 0, "transient reference releases the segment after close");

    CHECK(proc_shm_clone(&child, &owner) == 0 && refs[0] == 2,
          "fork inherits the open handle with its own reference");
    CHECK(proc_shm_close(&owner, h) == 0 && refs[0] == 1,
          "parent close does not revoke the child's inherited handle");
    sh = proc_shm_acquire(&child, h, 1, &writable);
    CHECK(sh == 0 && refs[0] == 2, "child retains the inherited write right");
    CHECK(writable == 1, "child retains its handle's mprotect ceiling");
    shm_put(sh);
    int before = locks;
    proc_shm_close_all(&child);
    CHECK(locks == before + 1, "exit detaches all SHM handles with one lock");
    CHECK(refs[0] == 0, "process exit releases inherited handles");

    printf("shmfd_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
