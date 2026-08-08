#ifndef EXECHOST_PROT_H
#define EXECHOST_PROT_H
/* PTE_NX and the predicate that says whether setting it is legal. The host
 * harness can answer 1 to cpu_prot_nx_usable(), which the kernel cannot yet --
 * see the NX report. That is the point of having it here: the no-execute half
 * of W^X is exercised in this test today. */
#define PTE_NX (1ull << 63)
int cpu_prot_nx(void);
int cpu_prot_nx_usable(void);
int cpu_prot_smep(void);
int cpu_prot_smap(void);
#endif
