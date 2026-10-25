/* A wasm export must be interruptible by the host.
 *
 * wasm_invoke runs a whole call inside one native function, so a host watchdog
 * driven by its own interpreter (QuickJS polls every 10,000 branches and calls)
 * never gets a poll while a module runs: `(loop (br 0))` simply never returned,
 * and the browser's CPU-slice watchdog -- which exists to stop exactly this --
 * could not fire. The interpreter now polls for the host on backward branches
 * and calls, every WASM_POLL_PERIOD events.
 *
 * -DWASM_NEGCTL_NO_POLL restores the old interpreter. Then the poll counts are
 * wrong and the spin export never returns; the alarm turns that hang into a
 * FAIL line instead of a stuck gate. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "wasm.h"
#include "wasm_exec.h"

static int checks, failures;
static void ok(int cond, const char *what)
{
	checks++;
	if (!cond) { failures++; printf("FAIL: %s\n", what); }
}
static void on_alarm(int s)
{
	(void)s;
	dprintf(1, "FAIL: spin never returned: the interpreter cannot be interrupted\n");
	dprintf(1, "wasm_interrupt_test: hung, %d failure(s)\n", failures + 1);
	_exit(1);
}

/* (func (export "spin") loop br 0 end) */
static const uint8_t SPIN[] = {
	0x00,0x61,0x73,0x6d, 0x01,0x00,0x00,0x00,
	0x01,0x04, 0x01,0x60,0x00,0x00,
	0x03,0x02, 0x01,0x00,
	0x07,0x08, 0x01, 0x04,'s','p','i','n', 0x00,0x00,
	0x0a,0x09, 0x01, 0x07, 0x00, 0x03,0x40, 0x0c,0x00, 0x0b, 0x0b,
};
/* (func (export "run") (param i32) (result i32) (local i32)
 *   loop  local1 = local1 + 1;  br_if 0 (local1 < param)  end  local1) */
static const uint8_t RUN[] = {
	0x00,0x61,0x73,0x6d, 0x01,0x00,0x00,0x00,
	0x01,0x06, 0x01,0x60,0x01,0x7f,0x01,0x7f,
	0x03,0x02, 0x01,0x00,
	0x07,0x07, 0x01, 0x03,'r','u','n', 0x00,0x00,
	0x0a,0x19, 0x01, 0x17, 0x01,0x01,0x7f,
	  0x03,0x40, 0x20,0x01, 0x41,0x01, 0x6a, 0x21,0x01,
	  0x20,0x01, 0x20,0x00, 0x49, 0x0d,0x00, 0x0b, 0x20,0x01, 0x0b,
};

static uint8_t arena_mem[1 << 20];
static int calls, stop_at;
static int hook(void *ctx) { (void)ctx; calls++; return stop_at && calls >= stop_at; }

static struct wasm_instance *load(const uint8_t *b, size_t n, struct wasm_module *m, struct wasm_arena *a)
{
	struct wasm_instance *in = 0;
	wasm_arena_init(a, arena_mem, sizeof arena_mem);
	if (wasm_load(b, (uint32_t)n, m, a)) return 0;
	if (wasm_instantiate(&in, m, a, 0, 0)) return 0;
	return in;
}

static int run_fn(struct wasm_instance *in, uint32_t n, uint32_t *result)
{
	uint32_t fi;
	union wasm_val arg, ret;
	if (!wasm_export_index(in, "run", 3, WASM_EXT_FUNC, &fi)) return -1;
	arg.bits = 0; arg.i32 = n; ret.bits = 0;
	int t = wasm_invoke(in, fi, &arg, &ret);
	*result = ret.i32;
	return t;
}

int main(void)
{
	struct wasm_module m; struct wasm_arena a;
	uint32_t res = 0;
	setvbuf(stdout, 0, _IONBF, 0);
	signal(SIGALRM, on_alarm);
	alarm(10);

	/* --- a bounded loop: polled at exactly the advertised rate ---------- */
	struct wasm_instance *in = load(RUN, sizeof RUN, &m, &a);
	ok(in != 0, "fixture: the counted-loop module loads");
	if (!in) return 2;

	calls = 0; stop_at = 0;
	ok(run_fn(in, 30, &res) == WASM_TRAP_NONE && res == 30, "no hook: run(30) returns 30");
	wasm_set_interrupt(in, hook, 0);
	calls = 0;
	ok(run_fn(in, 30, &res) == WASM_TRAP_NONE && res == 30 && calls == 0,
	   "a short run is never polled (30 events < one period)");
	calls = 0;
	/* 1 call + 24,999 backward branches = 25,000 poll events = 2 callbacks. */
	ok(run_fn(in, 25000, &res) == WASM_TRAP_NONE && res == 25000, "run(25000) completes when the hook allows");
	ok(calls == 2, "25,000 poll events produce exactly 2 callbacks at a 10,000 period");

	/* --- the hook can stop it ------------------------------------------- */
	calls = 0; stop_at = 2;
	int t = run_fn(in, 100000000u, &res);
	ok(t == WASM_TRAP_INTERRUPTED, "a long run is interrupted when the hook says so");
	ok(calls == 2, "...at the second callback, not later");
	ok(!wasm_trap_is_module_fault(t), "an interrupt is not the module's fault");
	ok(strstr(wasm_trapstr(t), "interrupt") != 0, "the trap is named");

	/* --- and the instance is still usable afterwards -------------------- */
	calls = 0; stop_at = 0;
	ok(run_fn(in, 5, &res) == WASM_TRAP_NONE && res == 5, "after an interrupt, the next call runs normally");

	/* --- the case that matters: a loop with no exit --------------------- */
	in = load(SPIN, sizeof SPIN, &m, &a);
	ok(in != 0, "fixture: the spin module loads");
	if (!in) return 2;
	uint32_t fi;
	ok(wasm_export_index(in, "spin", 4, WASM_EXT_FUNC, &fi), "spin is exported");
	wasm_set_interrupt(in, hook, 0);
	calls = 0; stop_at = 1;
	t = wasm_invoke(in, fi, 0, 0);
	ok(t == WASM_TRAP_INTERRUPTED, "an infinite loop is interrupted instead of hanging");
	ok(calls == 1, "...by the first callback");

	printf("wasm_interrupt_test: %d checks, %d failure(s)\n", checks, failures);
	return failures != 0;
}
