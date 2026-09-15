#ifndef LOGIT_FSBENCH_H
#define LOGIT_FSBENCH_H

/* /dev/fsbench -- the storage-path stopwatch.
 *
 * WHY A NODE AND NOT A HOST SCRIPT
 * --------------------------------
 * "Opening the Browser feels slow" is a claim about a 3 MB file being found,
 * read off a device, and copied into an address space. Three of those four
 * phases live inside this kernel, and the only honest way to say how long each
 * one takes is to time it where it happens, on the machine, against the real
 * device -- a host-side simulation of a block driver measures the simulation.
 *
 * kprof's SAMPLING profiler answers "which function is the machine in" without
 * any source change, and it is the right instrument for the phases this line
 * does not own (elf_load, the window manager's first paint). It cannot answer
 * "how many device round trips did reading this file cost, and what did one
 * cost", because that is a question about a named phase of one operation and a
 * flat address histogram has no way to apportion it. So this is the SPAN half
 * of the same measurement, driven from ring 3:
 *
 *     echo "launch /browser.aex" > /dev/fsbench
 *     cat /dev/fsbench
 *
 * Every number is a median of repeated runs with the min and max printed beside
 * it, because the host runs other QEMU instances concurrently and a single
 * sample is weather, not a measurement.
 *
 * COMMANDS
 *   blk   <sectors> <reps>      raw device reads of <sectors> at a time
 *   file  <path> [reps]         whole-file read, cold cache and warm
 *   launch <path> [reps]        what wm_launch does: size, allocate, read
 *   cache                       buffer-cache counters
 *   all [reps]                  the launch table: big app, small app, blk sizes
 *   openmax                     the largest file this machine can put behind a
 *                               descriptor, as a bracket (see bench_openmax)
 *   openfd <path>               what ONE open descriptor costs in kernel heap,
 *                               with a checksum proving it still reads the file
 *
 * The report also goes to the serial log with a "[bench]" prefix, so a boot
 * harness can grep it without a shell round trip. */

int  fsbench_command(const char *buf, int len);   /* 0 = understood */
int  fsbench_render(char *out, int max);
int  fsbench_len(void);

#endif /* LOGIT_FSBENCH_H */
