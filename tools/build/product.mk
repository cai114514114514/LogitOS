# SPDX-License-Identifier: MIT
# Product-only configuration domains. Keep host probes' many sanitizer and
# negative-control variants separate; sharing their objects would change code.
# Include this after the test fragments, which also supply shipped programs.
include tools/build/config.mk

PRODUCT_C_OBJECTS :=
define product_c
$(call build_config,$(1),CC,$(3),$(2))
$(2): $(call config_stamp,$(1))
PRODUCT_C_OBJECTS += $(2)
endef

KERNEL_C_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(C_SRC))
$(eval $(call product_c,kernel,$(KERNEL_C_OBJECTS),CFLAGS))
ACPI_C_OBJECTS := $(filter $(BUILD)/third_party/uacpi/% $(BUILD)/c/drivers/power/%,$(KERNEL_C_OBJECTS))
$(eval $(call build_config,acpi,CC,UACPI_CFLAGS,$(ACPI_C_OBJECTS)))
$(ACPI_C_OBJECTS): $(call config_stamp,acpi)

# These all use UCFLAGS directly. Special recipes below have their own stamp,
# so changing a browser or Preview option does not invalidate the whole OS.
# Register macro-produced objects at their recipes, not from the installation
# list: CLI also contains probes whose real objects live in separate families.
APP_C_OBJECTS := $(APP_OBJECTS) $(BUILD)/apps/studio.o $(BUILD)/apps/terminal.o $(BUILD)/apps/aui.o
CLI_C_OBJECTS := $(CLI_OBJECTS)
AS_C_OBJECTS := $(patsubst %.c,$(BUILD)/asobj/%.o,$(AS_C) $(AS_LIBC))
AS_TEST_OBJECTS := $(BUILD)/asobj/tests/unit/libctest_main.o $(BUILD)/asobj/tests/unit/closefull_main.o
MEDIA_C_OBJECTS := $(VID_OBJ) $(AUD_OBJ) $(IMGCHK_OBJ) $(MED_OBJ) \
  $(BUILD)/vidobj/tests/guest/video/vidcheck.o $(BUILD)/audobj/tests/guest/audio/audiocheck.o $(BUILD)/imgobj/imgcheck.o
PLAIN_C_OBJECTS := $(APP_C_OBJECTS) $(CLI_C_OBJECTS) $(AS_C_OBJECTS) $(AS_TEST_OBJECTS) \
  $(GFX_OBJ) $(STUDIO_OBJ) $(CLOCK_UI_OBJ) $(PKGV_OBJ) $(R3NET_OBJ) \
  $(BUILD)/r3netobj/tests/guest/net/h2check.o $(MEDIA_C_OBJECTS) $(LIBM_OBJ) \
  $(BUILD)/apps/as/editor/completion.o $(BUILD)/apps/as/common/version.o
$(eval $(call product_c,user,$(sort $(PLAIN_C_OBJECTS)),UCFLAGS))
$(eval $(call product_c,preview,$(BUILD)/apps/preview.o,UCFLAGS PREVIEW_CF))
$(eval $(call product_c,login,$(LOGIN_OBJ) $(GREETER_OBJ),LOGIN_CFLAGS))
$(eval $(call product_c,net-cli,$(NET_CLI_OBJ),UCFLAGS))

ENGINE_C_OBJECTS := $(patsubst %.c,$(BUILD)/jsobj/%.o,$(ENGINE_SRCS))
$(eval $(call product_c,js-engine,$(ENGINE_C_OBJECTS),JS_CF))
$(eval $(call product_c,browser-js,$(BROWSER_JS_OBJ),BROWSER_JS_CF))
BROWSER_PASSIVE := $(BUILD)/browserobj/c/apps/gui/browser/frame/passive.o
BROWSER_POLICY := $(BUILD)/browserobj/c/apps/gui/browser/frame/policy.o
$(eval $(call product_c,browser,$(filter-out $(BROWSER_PASSIVE) $(BROWSER_POLICY),$(BROWSER_OBJ)),UCFLAGS CSS_INC))
$(eval $(call product_c,browser-passive,$(BROWSER_PASSIVE),BROWSER_JS_CF CSS_INC))
$(eval $(call product_c,browser-policy,$(BROWSER_POLICY),UCFLAGS CSS_INC JS_INC))
$(eval $(call product_c,css,$(CSS_OBJ),UCFLAGS CSS_INC))
$(eval $(call product_c,browser-heap,$(BUILD)/browserobj/malloc_big.o,UCFLAGS))

# Add-on domains need not repeat their base flags. They invalidate only objects
# whose real command receives the option (including kernel and browser codecs).
CODEC_C_OBJECTS := $(KERNEL_IMG_CODEC_OBJ) $(BROWSER_IMG_CODEC_OBJ) $(VID_OBJ) $(AUD_OBJ) $(IMGCHK_OBJ)
$(eval $(call build_config,codec,CC,CODEC_OPT,$(CODEC_C_OBJECTS)))
$(CODEC_C_OBJECTS): $(call config_stamp,codec)

# Product fragments compile these with their own extra options. Register their
# public object lists, retaining distinct compiler domains instead of treating
# source-path equality as proof that two objects can be shared.
$(eval $(call product_c,tcc,$(TCC_OBJ),UCFLAGS TCC_LOGIT_DEFS))
AGENT_C_OBJECTS := $(AGENT_SDK_OBJ) \
  $(patsubst %.c,$(AGENT_DIR)/%.o,$(wildcard c/apps/libc/src/*.c)) \
  $(addprefix $(AGENT_DIR)/,c/apps/gui/assistant/assistant.o c/apps/agent/agentd.o \
  c/apps/agent/userfs.o c/lib/agent/model.o c/lib/agent/model_config.o c/net/http/http1.o)
$(eval $(call product_c,agent,$(AGENT_C_OBJECTS),UCFLAGS))
$(eval $(call build_config,agent-broker,CC,AGENT_BROKER_FLAGS,$(AGENT_DIR)/c/apps/agent/agentd.o))
$(AGENT_DIR)/c/apps/agent/agentd.o: $(call config_stamp,agent-broker)
$(eval $(call product_c,model,$(LM_NN_OBJ) $(BUILD)/lmobj/c/apps/lm/lm.o,UCFLAGS))

include tools/build/variants.mk
include tools/build/extras.mk

# Kernel assembly and its final link use different tools and options from C.
# Do not send assembly objects through the C-only missing-depfile recovery.
KERNEL_ASM_OBJECTS := $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRC))
$(eval $(call build_config,kernel-asm,ASM,ASFLAGS,$(KERNEL_ASM_OBJECTS)))
$(KERNEL_ASM_OBJECTS): $(call config_stamp,kernel-asm)
$(eval $(call build_config,kernel-link,LD,LDFLAGS,$(KERNEL)))
$(KERNEL): $(call config_stamp,kernel-link)

# Missing .d used to mean "headers no longer matter" for a surviving .o. Force
# that object through its real recipe once to recover its dependency closure.
# This also upgrades old build trees when removing all-headers prerequisites.
PRODUCT_C_OBJECTS := $(sort $(PRODUCT_C_OBJECTS))
PRODUCT_DEPFILES := $(wildcard $(PRODUCT_C_OBJECTS:.o=.d))
PRODUCT_WITHOUT_DEPS := $(filter-out $(PRODUCT_DEPFILES:.d=.o),$(PRODUCT_C_OBJECTS))
.PHONY: product-missing-deps
$(PRODUCT_WITHOUT_DEPS): product-missing-deps

$(eval $(call build_config_finish))
