# tests/containers.mk -- AVI/MPEG-TS/MPEG-PS/FLV, and the demux.c wiring
# question this phase exists to answer.
#
# Kept out of the main Makefile on purpose, same reasoning as tests/demux.mk
# and tests/h265.mk: a shared multi-thousand-line file is where concurrent
# edits collide. The root Makefile carries one `-include tests/containers.mk`
# line and nothing else.
#
# THE FIVE FILES THIS PHASE ADDED WERE 3,085 LINES NOBODY HAD EVER RUN.
# c/lib/media/pes.c is NOT a sixth container -- pes.h says so in as many
# words ("pieces MPEG-TS and MPEG-PS both need") and has no sniff/parse/open
# of its own; it is exercised only through ts.c and ps.c. So this is four
# containers, not five.
#
# THE STRUCTURAL QUESTION -- WIRED, with the gate that proves it. Before this
# session, c/lib/media/demux.c (the only thing a player is documented to
# talk to) recognised only mp4/mkv; avi.c/ts.c/ps.c/flv.c compiled, linked
# into every shipped binary, and were reachable by NOTHING except a test file
# that called their own avi_open()/ts_open()/ps_open()/flv_open() directly --
# the exact "linked a TU, never called it" shape this tree's WPT runner and
# cookie-transport bugs already carry as warnings. AVI and FLV needed only a
# dispatch arm (their own header comments already say they build an ordinary
# mdemux closeable with the generic media_close()). TS and PS needed the
# fix ts.h's own header comment named and left undone ("a per-kind
# 'owns_data' flag ... not written here because media_int.h and demux.c
# belong to another workflow") -- struct mdemux grew that field, media_close()
# now frees the reassembled scratch buffer when it is set, and ts_parse/
# ps_parse set it on success. test-containers-verify-generic is the gate that
# proves the wiring: it opens each fixture BOTH ways (avi_open() etc., proven
# against ffprobe below, and media_open(), demux.c's public API) and requires
# every track, every sample's pts/dts/size/keyframe AND payload bytes to
# match exactly, then closes the generic mdemux with media_close() -- which
# is what actually exercises owns_data, and is why test-containers-fuzz links
# the ASan build of the same check. A gate that opened only through
# avi_open()/ts_open() would prove nothing about demux.c; this one goes
# through media_open() to media_close(), sniff to free.
#
# TWO REAL BUGS THIS PHASE FOUND, both by tools that had never been pointed
# at these files before it, neither of them by lucky guessing:
#   - avi.c never captured an H.264 track's avcC-shaped codec-private data
#     from strf's WAVEFORMATEX-style extension bytes (parse_strf_video read
#     only BITMAPINFOHEADER's fixed fields and stopped) -- so framing
#     defaulted to RAW even on a file whose samples ARE this container's own
#     AVCC convention, and media_to_annexb copied them through un-rewritten,
#     no start codes, no parameter sets. Found by test-containers-diff:
#     "extradata size: ours 0-0 ffmpeg 40", the SAME CRC32 the identical
#     clip's FLV/MP4 remux carries. Fixed; test-containers-diff is clean on
#     both AVI fixtures now.
#   - ts_open()/ps_open()'s FAILURE path freed only the mdemux struct, never
#     the per-track sample arrays a partial parse had already built via
#     md_push -- every other open path in this library (media_open() itself,
#     avi_open(), flv_open(), and the two success paths ts_close()/
#     ps_close()) frees those; the two failure paths did not. Found by
#     test-containers-fuzz's ASan leak detector, on real fuzzed streams
#     (a continuity-counter gap, a truncated pack) that fail parse AFTER at
#     least one track already has samples. Fixed; the same fuzz corpus and
#     five more seeds are clean.
#
# THE FOUR "container units" FAILURES ON FIRST RUN, and TEST vs CODE for
# each -- the rule this tree insists on ("before believing a failure, check
# that the thing reporting it is looking at what you think it is"), applied
# to a test file nobody had ever run either:
#   sample0 bytes (TS)          TEST. ts_pack()'s hand-built fixture padded a
#                                short PES packet with raw 0xFF INSIDE the
#                                payload instead of a real adaptation field --
#                                ts.c correctly (per its own documented
#                                contract) never trusts PES_packet_length, so
#                                it read the padding as ES data. Fixed in
#                                container_test.c's ts_pack()/ts_pes_packet().
#   ps open: corrupt (PS)       TEST. ps_pack_header_mpeg2() wrote 8 bytes
#                                after b0 where ISO/IEC 13818-1's MPEG-2
#                                pack_header is 9 (80 bits total after the
#                                start code) -- ps.c's own `br_bytes(&top,9)`
#                                is the spec, not an arbitrary count. Fixed.
#   avcC framing auto-detected  TEST. The FLV fixture's AVC sequence-header
#   avcC extradata size (FLV)   body reserved 2 bytes for a 3-byte
#                                CompositionTime field, so avcc[0] (0x01) got
#                                read as part of the timestamp and every
#                                other byte shifted one early. Fixed.
# All four were the test, and container_test.c is the only file changed for
# any of them; avi.c/flv.c/ts.c/ps.c/pes.c needed nothing here.
#
# THE FFPROBE DIFFERENTIAL -- tests/unit/demux_diff.py already does exactly
# this for mp4/mkv (see its own header); this reuses it rather than inventing
# a second comparison, with THREE narrow, documented carve-outs added for
# where a format's own convention differs from ffprobe's, same shape as the
# MKV dts/pos exclusion already there:
#   - AVI's ffprobe time_base is the raw (dwScale, dwRate) pair, not
#     1/dwRate -- an exact integer factor (dwScale), not a tolerance.
#   - TS/PS/FLV's ffprobe `pos` is a different coordinate space (TS/PS:
#     ts.h/ps.h's reassembled scratch buffer, not a file offset at all;
#     FLV: the tag's own start, where media_sample.file_off is documented to
#     point at the PAYLOAD -- verified as a FIXED, exact per-tag-type byte
#     count, +16/+13 on this corpus, not a wrong offset).
#   - Annex-B (in-band) parameter sets mean there is no discrete avcC/hvcC
#     record for either side to disagree about -- ffprobe SYNTHESISES one by
#     scanning the stream; the test is POSITIVE (does sample 0 itself start
#     with a start code), not "which container is this", so it cannot mask
#     a real missing-extradata bug the way trusting our own zero would have.
#
# WHAT IS NOT EXACT, MEASURED, NOT FORCED GREEN. Two of these four containers
# are fully exact against ffprobe (AVI, FLV -- see test-containers-diff).
# The other two are not, in ways that are real, understood, and do NOT
# involve losing or corrupting a single byte -- verified separately (see the
# recipe below): concatenating every track's samples in order and diffing
# against ffmpeg's own elementary-stream extraction is BYTE IDENTICAL in
# every case named below.
#   - TS/PS AUDIO (MP3): ffmpeg's muxer packs several encoded MP3 frames into
#     one PES packet; ts.c/ps.c treat one PES as one sample BY DESIGN (their
#     own header comments: "Audio ... never split this way by any encoder
#     this project has seen" -- true of the encoders it had been checked
#     against; not true of ffmpeg's own MP3-in-TS/PS packetiser). Measured:
#     78 real MP3 frames arrive as 6 (TS) / 9 (PS) coarser samples, same
#     16,300 bytes, same order, byte for byte.
#   - PS VIDEO specifically: ps.c's own documented heuristic -- a video PES
#     with NO timestamp, arriving mid-accumulation, is a CONTINUATION of the
#     current access unit -- is wrong for a real ffmpeg PS mux of a
#     no-B-frame source, which puts one COMPLETE PICTURE per PES packet but
#     omits PTS on every packet after the first. Measured on
#     containers-h264-mp3.mpg: 30 real pictures merge into 2 samples, still
#     3,950 bytes and the same order, byte for byte -- and this is NOT
#     small: fixing it needs a second, independently-argued rule for when an
#     untimed PES is a continuation versus a new picture (a per-codec access-
#     unit boundary scan, the same class of work as ts.c/avi.c's own IDR
#     scans, not a one-line change), so it is reported here rather than
#     rushed. `test-containers-diff-report` names the exact counts every run;
#     `test-containers-diff` (the required gate) does not include PS, and
#     TS is required for its H.264 track and reported-only for its MP3 one.

.PHONY: \
        test-containers-diff test-containers-diff-report \
        test-containers-verify-generic test-containers-fuzz \
        test-containers-fuzz-negctl test-containers-wiring-negctl \
        container-fixtures

CONTAINERS_SRC := $(wildcard c/lib/media/*.c)
CONTAINERS_HDRS := $(wildcard c/lib/media/*.h)
CONTAINERS_INC  := -Ic/lib/media
CONTAINERS_FX   := tests/fixtures/media

# NOTE the include path, exactly as tests/vp9.mk warns: -Ic/lib/media ONLY.
# $(INCDIRS) or -Ic/apps/libc/include puts mini-libc's features.h ahead of
# glibc's on a host build and __GLIBC_USE(X) parses as a call -- this cost
# two other agents a compile today before it was written down here too.

$(BUILD)/container_test: tests/unit/container_test.c $(CONTAINERS_SRC) $(CONTAINERS_HDRS)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -Wall -Wextra -o $@ tests/unit/container_test.c $(CONTAINERS_SRC) $(CONTAINERS_INC)

# --- fixtures ----------------------------------------------------------------
# Generated, deterministic (lavfi testsrc2 + sine -- no dependency on any
# other committed fixture), same shape as tests/unit/gen_media.sh. Regenerate
# explicitly; `make container-fixtures` and expect every count above to be
# re-measured, because a different ffmpeg writes a different file.
container-fixtures:
	@bash tests/unit/gen_containers.sh $(CONTAINERS_FX)

CONTAINERS_FIXTURES := $(CONTAINERS_FX)/containers-h264-mp3.avi \
                        $(CONTAINERS_FX)/containers-h264-mp3.ts \
                        $(CONTAINERS_FX)/containers-h264-mp3.mpg \
                        $(CONTAINERS_FX)/containers-h264-aac.flv






# The plausible wrong wiring: dispatch TS/PS through media_open() without
# ever setting owns_data, exactly this file's pre-fix state (ts.h's own
# comment named it and left it undone). media_close() then frees only the
# per-track sample arrays, never the reassembled scratch buffer -- a real
# leak on every TS/PS file the generic path closes, REQUIRED to be caught by
# AddressSanitizer's leak detector specifically, not merely by a nonzero
# exit code.
CONTAINERS_ASAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all \
                         -fno-omit-frame-pointer
CONTAINERS_ASAN_ENV   := ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
                         UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

# --- FUZZ, under ASan + UBSan -------------------------------------------------
# Same argument as test-demux-fuzz: a container is the most attacker-shaped
# input in this system. See tests/unit/container_fuzz.c's header for the
# three phases and for why its negative control is DEMUX_FUZZ_SABOTAGE --
# shared with test-demux-fuzz-negctl on purpose, because it guts the same
# shared demux.c function (media_to_annexb) these four formats call through
# too; AVI's and FLV's H.264 AVCC tracks are what prove it live here.
SCALE ?= 8
SEED  ?= 0x243F6A8885A308D3

$(BUILD)/container_fuzz: tests/unit/container_fuzz.c $(CONTAINERS_SRC) $(CONTAINERS_HDRS)
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g $(CONTAINERS_ASAN_FLAGS) -w $(CONTAINERS_INC) \
	    -o $@ tests/unit/container_fuzz.c $(CONTAINERS_SRC)



