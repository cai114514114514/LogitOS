DEVICE_MEDIA_SRC = $(filter-out tests/unit/webapi_platform_test.c,$(PLATFORM_TEST_SRC)) tests/unit/device_media_test.c
DEVICE_MEDIA_DEPS = $(DEVICE_MEDIA_SRC) $(PLATFORM_MOD) tests/unit/webapi_platform_test.c $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
DEVICE_MEDIA_OUT = $(BUILD)/site-general/runtime
# Supply BOTH real matcher consumers before the archive in the negative link:
# select.c answers matchMedia, hash.c decides stylesheet @media. Dropping only
# one would let the suite accidentally measure the other unmodified door.
DEVICE_MEDIA_MATCH_SRC = third_party/css/libcss/src/select/select.c third_party/css/libcss/src/select/hash.c
.PHONY: test-device-media test-device-media-negctl
$(DEVICE_MEDIA_OUT)/device_media_test: $(DEVICE_MEDIA_DEPS)
	@mkdir -p $(DEVICE_MEDIA_OUT)
	$(CC) -O2 -w $(PLATFORM_CF) -o $@ $(DEVICE_MEDIA_SRC) $(PLATFORM_MOD) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(DEVICE_MEDIA_OUT)/device_media_negctl: $(DEVICE_MEDIA_DEPS) $(DEVICE_MEDIA_MATCH_SRC) third_party/css/libcss/src/select/mq.h
	@mkdir -p $(DEVICE_MEDIA_OUT)
	$(CC) -O2 -w $(PLATFORM_CF) -D_ALIGNED= -DWITHOUT_ICONV_FILTER -DCSS_DEVICE_MEDIA_NEGCTL -o $@ $(DEVICE_MEDIA_SRC) $(PLATFORM_MOD) $(HTML_PARSER_SRC) $(QJS_SRC) $(DEVICE_MEDIA_MATCH_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-device-media-negctl: $(DEVICE_MEDIA_OUT)/device_media_negctl
	@rc=0; $< > $(DEVICE_MEDIA_OUT)/device_media_negctl.log 2>&1 || rc=$$?; cat $(DEVICE_MEDIA_OUT)/device_media_negctl.log; test $$rc -eq 1 && grep -q '^FAIL: real device-width search converges' $(DEVICE_MEDIA_OUT)/device_media_negctl.log && grep -q '^FAIL: device media query selects' $(DEVICE_MEDIA_OUT)/device_media_negctl.log
test-device-media: test-device-media-negctl $(DEVICE_MEDIA_OUT)/device_media_test
	@$(DEVICE_MEDIA_OUT)/device_media_test

$(DEVICE_MEDIA_OUT)/motion_media_negctl: $(DEVICE_MEDIA_DEPS) $(DEVICE_MEDIA_MATCH_SRC) third_party/css/libcss/src/select/mq.h
	@mkdir -p $(DEVICE_MEDIA_OUT)
	$(CC) -O2 -w $(PLATFORM_CF) -D_ALIGNED= -DWITHOUT_ICONV_FILTER -DOPENLOGIT_REDUCED_MOTION_DISABLED -o $@ $(DEVICE_MEDIA_SRC) $(PLATFORM_MOD) $(HTML_PARSER_SRC) $(QJS_SRC) $(DEVICE_MEDIA_MATCH_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-motion-media-negctl: $(DEVICE_MEDIA_OUT)/motion_media_negctl
	@rc=0; $< > $(DEVICE_MEDIA_OUT)/motion_media_negctl.log 2>&1 || rc=$$?; test $$rc -eq 1 && rg '^FAIL: motion (preference updates|media query selects)' $(DEVICE_MEDIA_OUT)/motion_media_negctl.log
test-device-media: test-motion-media-negctl
.PHONY: test-motion-media-negctl
