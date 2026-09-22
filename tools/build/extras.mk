# SPDX-License-Identifier: MIT
# Programs added by test fragments also ship on the ordinary disk. Reuse their
# source-to-object lists: the product registry must not become a second copy of
# the HTTPS crypto, native runtime, or SDK source inventories.
$(eval $(call product_c,https,$(HTTPS_OBJ),UCFLAGS INCDIRS))
$(eval $(call product_c,sshd,$(SSHD_OBJS),UCFLAGS INCDIRS))
$(eval $(call product_c,native-runtime,$(AS_NATIVE_OBJS),UCFLAGS))
$(eval $(call product_c,native-capture,$(BUILD)/as-typed-capture.o,UCFLAGS))
$(eval $(call product_c,mse,$(MSECHK_OBJ),UCFLAGS))

# The SDK example inventory includes the scene/vector subdirectories; preserving
# their relative paths avoids merging unrelated main.o or ui.o files. olscc and
# lslcc are explicit compiler entry targets in openlogit_sdk.mk, not examples.
SDK_C_OBJECTS := $(OPENLOGIT_3D_OBJ) \
  $(patsubst examples/openlogit/%.c,$(BUILD)/sdk/%.o,$(filter %.c,$(OPENLOGIT_EXAMPLES))) \
  $(BUILD)/sdk/olscc.o $(BUILD)/sdk/lslcc.o
$(eval $(call product_c,openlogit-sdk,$(sort $(SDK_C_OBJECTS)),UCFLAGS))

GUEST_VIDEO_OBJECTS := $(addprefix $(BUILD)/vidobj/tests/guest/video/,$(addsuffix .o,vidcheck265 vidbench))
$(eval $(call product_c,guest-video,$(GUEST_VIDEO_OBJECTS),UCFLAGS CODEC_OPT))
GUEST_JS_OBJECTS := $(addprefix $(BUILD)/jsobj/tests/unit/,$(addsuffix .o,js_bench js_sem_probe))
$(eval $(call product_c,guest-js,$(GUEST_JS_OBJECTS),JS_CF))
GUEST_HEAP_OBJECTS := $(BUILD)/jsbenchobj/malloc_big.o $(BUILD)/jssemobj/malloc_big.o
$(eval $(call product_c,guest-heaps,$(GUEST_HEAP_OBJECTS),UCFLAGS))

# These are real object targets after the ch/preview link recipes were split.
# Registering their old recipe side effects would have created targets that
# looked rebuildable but never invoked the compiler when a header changed.
$(eval $(call product_c,chat,$(CH_OBJ),UCFLAGS))
PREVIEW_CLI_OBJECTS := $(addprefix $(BUILD)/apps/,$(addsuffix .o,previewplay previewnegctl previewneg))
$(eval $(call product_c,preview-probes,$(PREVIEW_CLI_OBJECTS),UCFLAGS))

# sysroot.mk deliberately removes -MMD/-MP. Putting these in PRODUCT_C_OBJECTS
# would therefore force them forever through missing-depfile recovery. Track
# their commands directly, and keep the .S member out of the C registry too.
# All three are cross-compiled by CC, despite residing in a host staging tree;
# the host asc executable does not produce any of these objects.
SYSROOT_C_OBJECTS := $(patsubst $(SYSROOT_TCCSRC)/lib/%.c,$(SYSROOT_WORK)/tcc1/%.o,$(filter %.c,$(SYSROOT_TCC1_SRC)))
SYSROOT_ASM_OBJECTS := $(filter-out $(SYSROOT_C_OBJECTS),$(SYSROOT_TCC1_OBJ))
SYSROOT_ASM_FLAGS := $(filter-out -std=c11,$(SYSROOT_UCFLAGS))
$(eval $(call build_config,sysroot-tcc,CC,SYSROOT_UCFLAGS,$(SYSROOT_C_OBJECTS)))
$(SYSROOT_C_OBJECTS): $(call config_stamp,sysroot-tcc)
$(eval $(call build_config,sysroot-asm,CC,SYSROOT_ASM_FLAGS,$(SYSROOT_ASM_OBJECTS)))
$(SYSROOT_ASM_OBJECTS): $(call config_stamp,sysroot-asm)
