# Card-family drivers share the existing PCM ABI and mixer. Host gates cover
# register/ownership behavior; the guest gate must inspect captured PCM.
-include tests/drivers/audio/ac97/test.mk
-include tests/drivers/audio/es1370/test.mk
-include tests/drivers/audio/framework/tests.mk
-include tests/drivers/audio/playback/tests.mk
-include tests/drivers/audio/capture/tests.mk

.PHONY: test-audio-cards-host test-audio-cards-oracle test-audio-cards-os

test-audio-pcm: test-audio-pcm-negctl
test-audio-cards-host: test-audio-pcm test-hda-x79-host test-ac97-host test-es1370-host test-capture-framework test-playback-framework

test-audio-cards-oracle:
	@python3 tests/drivers/audio/guest.py --self-test

test-audio-cards-os: test-audio-cards-host test-audio-cards-oracle $(ISO) $(DISK)
	@mkdir -p "$(BUILD)/audio-guests"
	@set -e; run=$$(mktemp -d "$(BUILD)/audio-guests/run.XXXXXX"); \
	 echo "Audio guest evidence: $$run"; \
	 for card in intel-hda AC97 ES1370; do \
	   python3 tests/drivers/audio/guest.py --iso "$(ISO)" --disk "$(DISK)" \
	     --device "$$card" --output "$$run/$$card"; \
	 done

ci-host: test-audio-cards-host test-audio-cards-oracle
test-driver-host: test-audio-cards-host test-audio-cards-oracle
ci-host test-driver-host: test-audio-capture-oracle
