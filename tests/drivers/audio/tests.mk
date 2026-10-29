# Card-family drivers share the existing PCM ABI and mixer. Host gates cover
# register/ownership behavior; the guest gate must inspect captured PCM.
-include tests/drivers/audio/ac97/test.mk
-include tests/drivers/audio/es1370/test.mk
-include tests/drivers/audio/framework/tests.mk
-include tests/drivers/audio/playback/tests.mk
-include tests/drivers/audio/capture/tests.mk

.PHONY: test-audio-cards-host test-audio-cards-oracle

test-audio-pcm: test-audio-pcm-negctl
test-audio-cards-host: test-audio-pcm test-hda-x79-host test-ac97-host test-es1370-host test-capture-framework test-playback-framework

test-audio-cards-oracle:
	@python3 tests/drivers/audio/guest.py --self-test


ci-host: test-audio-cards-host test-audio-cards-oracle
test-driver-host: test-audio-cards-host test-audio-cards-oracle
ci-host test-driver-host: test-audio-capture-oracle
