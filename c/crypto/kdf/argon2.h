#ifndef LOGIT_KDF_ARGON2_H
#define LOGIT_KDF_ARGON2_H

#include <stdint.h>
#include <stddef.h>

/* Argon2 (RFC 9106, version 0x13 / "1.3") -- the PHC winner and the current
 * default recommendation for password hashing. See scrypt.h right next to
 * this file for why THIS machine's login record stays PBKDF2 today (short
 * version, pbkdf2.c's own: 512 MiB total RAM and a young reclaim path make a
 * KDF an unauthenticated caller can turn into an OOM condition a worse trade
 * here than elsewhere). Argon2id is memory-hard in the same sense scrypt is,
 * with two differences that motivated shipping it too rather than calling
 * scrypt "the" memory-hard KDF: (1) it defines three addressing modes and a
 * hybrid of two of them (Argon2id) that scrypt has no equivalent of, so it is
 * a genuinely different security argument, not a second encoding of the same
 * one; (2) it is what every modern password-hashing guidance (OWASP included,
 * the same source pbkdf2.c cites for its iteration count) now lists first.
 *
 * NO CONSUMER. This is a library primitive shipped with its own gate, not
 * wired into any trust path -- no c/crypto/trust, no TLS, no aex, and
 * specifically not c/apps/coreutils/login's password path, which is the
 * obvious place a reader will look for this and the one place it must not
 * appear from this workflow. See CLAUDE.md's rule on primitives vs. things
 * that make a false claim, and pbkdf2.c's header for the argument that would
 * have to be re-made, with THIS machine's numbers, before wiring it.
 *
 * THE THREE CONSTRUCTIONS, AND WHY ARGON2ID GETS ITS OWN FUNCTION RATHER THAN
 * A FLAG ON A SHARED ONE (see argon2.c's top comment for the trap this
 * guards): Argon2d fills every block using data-DEPENDENT addressing (the
 * reference index for block i comes from block i-1's own first 8 bytes) --
 * maximum resistance to a GPU/ASIC time-memory trade-off, but the access
 * pattern leaks through cache timing to anything sharing the machine.
 * Argon2i uses data-INDEPENDENT addressing (a separate pseudo-random stream
 * seeded from the public parameters, never from a block's contents) for
 * EVERY block of EVERY pass -- safe against that side channel, weaker
 * against the trade-off attack. Argon2id is not a third algorithm so much as
 * a policy for switching between the other two mid-run: Argon2i addressing
 * for the first two slices of pass 0 only (before an attacker with
 * co-resident timing access has seen enough of the run to build a useful
 * trade-off table), Argon2d addressing everywhere else. That switch point is
 * argon2.c's central hazard.
 *
 * CALLER-SUPPLIED SCRATCH, ALWAYS -- same rule as scrypt.h, same reason: no
 * allocator in the kernel. This file declares no static array. The caller
 * picks (t_cost, m_cost_kib, lanes), calls argon2_memory_blocks() to learn
 * exactly how many ARGON2_BLOCK_SIZE-byte blocks that rounds to (Argon2
 * silently rounds m_cost DOWN to a multiple of 4*lanes so every one of the 4
 * synchronisation slices is the same length -- that rounding is part of the
 * spec, not this file's decision, and argon2_memory_blocks() is what makes it
 * checkable rather than assumed), and passes a `mem` array of at least that
 * many blocks. If it is smaller, argon2() returns -1 and touches nothing.
 * THE ALTERNATIVE -- silently using fewer blocks than the caller asked for --
 * is worse than refusing: RFC 9106 4's whole memory-hardness argument is a
 * function of m, so a KDF that quietly ran at a smaller m than requested
 * returns a tag that is both WRONG (does not match what any other Argon2
 * implementation given the same parameters produces) and WEAKER than the
 * caller believed they were paying for, and nothing about the call site would
 * show either fact. */

#define ARGON2_BLOCK_SIZE   1024              /* bytes; RFC 9106 3.2 */
#define ARGON2_QWORDS       (ARGON2_BLOCK_SIZE / 8)   /* 128 */
#define ARGON2_VERSION_13   0x13
#define ARGON2_SYNC_POINTS  4                 /* slices per pass, fixed by the spec */

/* One 1024-byte working block. The caller's `mem` array is
 * argon2_memory_blocks(...) of these, contiguous -- memory[lane][pos] lives
 * at mem[lane * lane_length + pos] exactly as RFC 9106 3.2 describes it,
 * where lane_length = argon2_memory_blocks(...) / lanes. */
struct argon2_block { uint64_t v[ARGON2_QWORDS]; };

enum argon2_type { ARGON2_D = 0, ARGON2_I = 1, ARGON2_ID = 2 };

/* Exact block count argon2()'s (m_cost_kib, lanes) will use, AFTER the spec's
 * own rounding: m' = 4*lanes*floor(m_cost_kib / (4*lanes)), with a floor of
 * 8*lanes (RFC 9106 3.2's ARGON2_MIN_MEMORY = 2*SYNC_POINTS*lanes). t_cost
 * (the pass count) plays NO part in the memory size -- only in how many
 * times argon2() walks it -- so it is deliberately not a parameter here;
 * a signature that took it would invite a caller to believe otherwise.
 * Call this BEFORE sizing `mem` -- do not compute m_cost_kib as the block
 * count directly without applying the same rounding, which is exactly the
 * kind of "one jar, two doors" mismatch CLAUDE.md warns about; there is only
 * one door, and this is it. Returns 0 if lanes == 0. */
uint32_t argon2_memory_blocks(uint32_t m_cost_kib, uint32_t lanes);

/* The full construction (RFC 9106 3.2 + 3.3 + 3.4/3.5). `mem` must have at
 * least argon2_memory_blocks(t_cost, m_cost_kib, lanes) entries -- pass
 * mem_cap so the function can check that itself rather than trust the
 * caller. `secret` (the RFC's K, a pepper -- NOT the password) and `ad` may
 * each be NULL/0-length independently of one another; `pw`/`salt` follow the
 * same rule except RFC 9106 4 requires saltlen >= 8. `taglen` >= 4.
 *
 * Returns 0 on success (tag written), -1 on a bad parameter (lanes == 0,
 * t_cost == 0, saltlen < 8, taglen < 4), -2 if mem_cap is smaller than
 * argon2_memory_blocks() computes -- see the header comment above for why
 * that is refused rather than silently downgraded.
 *
 * Wipes the argon2_memory_blocks(m_cost_kib, lanes) blocks of `mem` that were
 * actually used (not the whole of mem_cap, if the caller passed a larger
 * buffer) before returning 0 -- every block is derived from the password.
 * On -1 or -2 `mem` is untouched: wiping a buffer this call never wrote to
 * would silently destroy a caller's unrelated data on an error path that is
 * not this function's business. */
int argon2(enum argon2_type type,
           const uint8_t *pw, uint32_t pwlen,
           const uint8_t *salt, uint32_t saltlen,
           const uint8_t *secret, uint32_t secretlen,
           const uint8_t *ad, uint32_t adlen,
           uint32_t t_cost, uint32_t m_cost_kib, uint32_t lanes,
           struct argon2_block *mem, uint32_t mem_cap,
           uint8_t *tag, uint32_t taglen);

#endif
