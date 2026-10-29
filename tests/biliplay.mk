# tests/biliplay.mk -- Bilibili's first-video acceptance gate.
#
# This is deliberately a browser gate, not another decoder test. The specimen
# bytes are BV1GJ411x7h7 (see tests/fixtures/biliplay/PROVENANCE.md), but the
# assertion crosses the page, fetch, MediaSource, split DASH SourceBuffers,
# demux, H.264/AAC, A/V clock, compositor and HDA paths. It counts frames that
# were SHOWN twice over time and non-silent PCM captured outside the guest; a
# screenshot or a decoder CRC cannot distinguish playback from a first-frame
# stall.
#
# Offline is the deterministic CI ratchet: it serves the captured specimen's
# real video bytes locally, through the same browser/network/MSE route. Live is
# the owner's requested finish line and drives the public Bilibili page with an
# honest UA. Nothing here branches product code on bilibili or spoofs support.

.PHONY: test-biliplay test-biliplay-offline test-biliplay-offline-negctl \
        test-biliplay-live

BILIPLAY_FX     := tests/fixtures/biliplay
BILIPLAY_FILES  := $(wildcard $(BILIPLAY_FX)/*)
BILIPLAY_DRIVER := tests/qmp/qmp_biliplay_page.py

# A control that traverses the identical machine and page but withholds every
# video media segment (the init segment and the full audio side still flow).
# The positive gate must catch this specifically at decoded=0, not merely see
# QEMU or networking fail. This costs a second boot because a host-only control
# cannot prove the browser-side counter is connected to the real decoder.
test-biliplay-offline-negctl: $(ISO) $(DISK) $(BILIPLAY_FILES) $(BILIPLAY_DRIVER)
	@mkdir -p $(BUILD)
	@if python3 $(BILIPLAY_DRIVER) $(ISO) $(DISK) --mode offline \
	        --control no-video --secs 90 >$(BUILD)/biliplay-negctl.log 2>&1; then \
	    echo "NEGCTL-FAIL: Bilibili playback passed with no video media segments"; \
	    exit 1; \
	 elif grep -Fq "FAIL: the specimen's H.264 decoded (0 pictures)" \
	        $(BUILD)/biliplay-negctl.log && \
	      grep -Fq "BILIPLAY-OFFLINE-FAIL" $(BUILD)/biliplay-negctl.log; then \
	    echo "negctl: withholding video segments is caught at decoded=0"; \
	 else \
	    echo "NEGCTL-FAIL: the sabotaged run failed for an unrelated reason"; \
	    tail -60 $(BUILD)/biliplay-negctl.log; \
	    exit 1; \
	 fi

test-biliplay-offline: test-biliplay-offline-negctl $(ISO) $(DISK) \
                       $(BILIPLAY_FILES) $(BILIPLAY_DRIVER)
	@python3 $(BILIPLAY_DRIVER) $(ISO) $(DISK) --mode offline --secs 120

test-biliplay: test-biliplay-offline

# Intentionally not on CI: this is a live public-site measurement whose result
# may be connectivity, CDN refusal or a newly exposed browser API gap. It must
# report those honestly rather than turn external unavailability green.
test-biliplay-live: test-biliplay-offline $(ISO) $(DISK) $(BILIPLAY_DRIVER)
	@python3 $(BILIPLAY_DRIVER) $(ISO) $(DISK) --mode live --secs 180

ci-boot: test-biliplay-offline
