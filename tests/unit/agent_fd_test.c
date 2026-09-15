/* SPDX-License-Identifier: MIT */
/* file.c is linked whole; proc_fd_* bodies are extracted without edits by the
 * runner. Only platform services come from the established pollhost model. */
#include <stdio.h>
#include <stdlib.h>
#include "file.h"
#include "proc.h"

void hostsched_init(void);
static unsigned checks, file_lock_entries;
#define CHECK(c, label) do { checks++; if (!(c)) { fprintf(stderr,"FAIL: %s\n",label); exit(1); } } while (0)

/* The file TU calls this observer in place of its architecture irqsave
 * wrapper. It still takes the real host ticket lock. Thus removing the lock
 * from the helper is observable without relying on a timing race. */
uint64_t agent_file_lock(spinlock_t *lock)
{ file_lock_entries++; return spin_lock_irqsave(lock); }

int main(void)
{
    hostsched_init();file_init();
    struct proc parent={0},child={0};
    parent.fd_lock=(spinlock_t)SPINLOCK_INIT;child.fd_lock=(spinlock_t)SPINLOCK_INIT;
    struct file *f=file_alloc();CHECK(f,"handoff: allocate description");
    int fd=proc_fd_alloc(&parent,f);CHECK(fd==0,"handoff: parent descriptor installed");
    struct file *held=proc_fd_acquire(&parent,fd);CHECK(held==f,"handoff: acquire transfer reference");
    unsigned entered=file_lock_entries;
    CHECK(file_refs_equal(held,2),"references: table and transfer reference counted");
    CHECK(file_lock_entries==entered+1,"references: query participates in file lock");
    CHECK(!file_refs_equal(NULL,2)&&!file_refs_equal(held,0),"references: invalid query refused");

    struct file *temporary=proc_fd_acquire(&parent,fd);
    CHECK(temporary==f,"handoff: temporary operation acquires reference");
    CHECK(!proc_fd_take_exclusive(&parent,fd,held)&&parent.fd[fd]==f,
          "handoff: temporary reference keeps parent descriptor");
    CHECK(file_refs_equal(held,3),"handoff: failed transfer leaves all references");
    file_close(temporary);
    CHECK(proc_fd_dup2(&parent,fd,5)==5,"handoff: duplicate descriptor created");
    CHECK(!proc_fd_take_exclusive(&parent,fd,held)&&parent.fd[fd]==f,
          "handoff: duplicate descriptor prevents transfer");
    CHECK(proc_fd_close(&parent,5)==0,"handoff: duplicate descriptor closed");
    proc_fd_clone(&child,&parent);
    CHECK(!proc_fd_take_exclusive(&parent,fd,held)&&parent.fd[fd]==f,
          "handoff: fork reference prevents transfer");
    proc_fd_close_all(&child);
    CHECK(file_refs_equal(held,2),"handoff: alias cleanup restores transfer pair");
    CHECK(!proc_fd_take_exclusive(&parent,fd+1,held),"handoff: wrong descriptor refused");
    CHECK(proc_fd_take_exclusive(&parent,fd,held)&&!parent.fd[fd],"handoff: exclusive descriptor transferred");
    CHECK(file_refs_equal(held,1),"handoff: receiver owns exactly one reference");
    struct file *replacement=file_alloc();CHECK(replacement&&replacement!=held,"handoff: live receiver cannot be recycled");
    CHECK(proc_fd_alloc(&parent,replacement)==fd,"handoff: old descriptor number may be reused");
    CHECK(!proc_fd_take_exclusive(&parent,fd,held)&&parent.fd[fd]==replacement,
          "handoff: stale identity cannot take replacement descriptor");
    CHECK(file_close(held)==0,"handoff: receiver closes its last reference");
    CHECK(file_refs_equal(replacement,1),"handoff: receiver cleanup preserves replacement");
    proc_fd_close_all(&parent);
    printf("AGENT_FD checks=%u failures=0\n",checks);return 0;
}
