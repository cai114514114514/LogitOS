/* c/lib/media/ps.h -- MPEG Program Stream (.mpg/.vob/.mpeg) demuxer, entries.
 *
 * WIRED -- same status as avi.h/ts.h: media_sniff()/media_open() dispatch
 * here as MEDIA_CONT_PS. ps_open() also still exists as a direct entry
 * point; either way the result is a working mdemux.
 *
 * MEMORY OWNERSHIP: the same story as ts.h, and for the same underlying
 * reason -- a video access unit can be split across more than one PES packet
 * (see ps.c's continuation heuristic), so a sample is not always one
 * contiguous run of file bytes. ps_parse reassembles into a scratch buffer
 * and points m->data at it. struct mdemux's `owns_data` flag (media_int.h)
 * is what makes the GENERIC media_close() safe on this now too -- ps_parse
 * sets it on success, and media_close() frees `data` only when it is set --
 * so media_open()'s dispatch (which calls ps_parse directly) closes
 * correctly with the ordinary media_close(). ps_open()/ps_close() still
 * exist and are still paired the same way as a direct entry point; pass a
 * ps_open() result to media_close() only through the generic dispatch's own
 * mdemux, never mix the two on one buffer. See ts.h's longer version of this
 * note, which applies here unchanged except that PS's fragmentation is the
 * exception (most real files never split a frame) rather than TS's rule
 * (every file does, for every frame, because 184-byte payloads are far
 * smaller than a coded picture).
 *
 * ONE MORE THING FOUND WHILE WIRING THIS IN, worth knowing before trusting
 * ps.c's video sample boundaries against a REAL encoder: the continuation
 * heuristic above ("a video PES with no timestamp, arriving mid-accumulation,
 * is the rest of the current access unit") is wrong for at least one real
 * muxer -- ffmpeg's own generic `-f mpeg` output for a no-B-frame H.264
 * source puts one COMPLETE PICTURE per PES packet but omits PTS on every
 * packet after the first, so this heuristic merges 30 real pictures into 2
 * samples. No byte is lost (concatenated output is byte-identical to
 * ffmpeg's own elementary-stream extraction, verified) -- only the AU
 * boundary is wrong. See tests/containers.mk's header for the measurement
 * and why it is reported rather than rushed into a fix.
 */
#ifndef LOGIT_PS_H
#define LOGIT_PS_H

#include "media_int.h"

/* By content: a pack_start_code (00 00 01 BA) at the very start of the file.
 * (Some PS files open with a system_header or PSM instead of a pack; that
 * is legal but is not how any encoder in this project's own gate -- or any
 * mainstream one -- actually writes a file, so it is not what is sniffed
 * for; a file this library cannot open still fails MEDIA_ERR_CORRUPT rather
 * than silently misreading, which is the property that matters.) */
int ps_sniff(const uint8_t *data, long len);

int ps_parse(mdemux *m);

mdemux *ps_open(const uint8_t *data, long len, int *err);

/* MUST be used instead of media_close() for a ps_open() result. */
void ps_close(mdemux *m);

#endif /* LOGIT_PS_H */
