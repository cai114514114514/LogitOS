# SPDX-License-Identifier: MIT
# Include after every product/test fragment and before product.mk finalizes its
# C-object inventory. Target-specific flags do not appear in a global CFLAGS or
# JS_CF snapshot. Register the controlling option separately, on BOTH its on and
# off invocations, or returning to a normal build can retain a control object.
#
# This consumer map must change together with the target-specific assignments in
# the named fragments below. It adds dependencies, never compiler flags: the real
# recipe remains the authority for how each object is compiled.
define product_variant
$(call build_config,$(1),CC,$(3),$(2))
$(2): $(call config_stamp,$(1))
endef

# tests/runtime_diagnostics.mk appends to five JS objects and to the passive
# frame's UCFLAGS. Both compiler domains need the same on/off provenance.
DIAGNOSTIC_OBJECTS := $(addprefix $(BUILD)/jsobj/c/apps/gui/browser/js/,\
  workers/worker.o runtime/webapi.o wasm/wasm.o workers/ports.o dom/dom.o) \
  $(BUILD)/browserobj/c/apps/gui/browser/frame/passive.o
$(eval $(call product_variant,runtime-diagnostics,$(DIAGNOSTIC_OBJECTS),RUNTIME_DIAGNOSTICS))

# ANIMNOCLK also changes global CFLAGS, already covered by the kernel domain;
# its JS consumer has frozen BROWSER_JS_CF and therefore needs this extra edge.
$(eval $(call product_variant,animation-clock,$(BUILD)/jsobj/c/apps/gui/browser/js/style/anim.o,ANIMNOCLK))
$(eval $(call product_variant,first-token,$(BUILD)/jsobj/c/apps/gui/browser/js/runtime/webapi.o,FTNOSTREAM))

# These replace the old per-feature phony/cmp stamps. The shared helper also
# covers interrupted rebuilds and Make 3.81's whole-second comparisons, while
# keeping make -n faithful when the option has not changed.
$(eval $(call product_variant,image-geometry,$(BUILD)/jsobj/c/apps/gui/browser/app/main.o,BROWSER_IMAGE_GEOMETRY_NEGCTL))
$(eval $(call product_variant,browser-visibility,$(BUILD)/jsobj/c/apps/gui/browser/js/dom/dom.o,BROWSER_TRACE_VISIBILITY))

# tests/pty.mk, usb_storage.mk and pci_intx_guest.mk alter these kernel targets.
# Separate groups retain which switch changed; the USB and PCI hooks replace
# the same dev_dump call and must not collapse into one anonymous flag value.
$(eval $(call product_variant,pty-control,$(BUILD)/c/kernel/exec/fd/pty.o,PTY_CONTROL))
$(eval $(call product_variant,usb-storage-hook,$(BUILD)/c/kernel/init/kmain.o,USBMSCVERIFY))
$(eval $(call product_variant,pci-intx-hook,$(BUILD)/c/kernel/init/kmain.o,PCIINTXGUEST))
$(eval $(call product_variant,pci-intx-control,$(BUILD)/c/kernel/pci/pci_msi.o,PCIINTXGUEST PCIINTXGUEST_NEG))

# These opt-in C objects enter OBJ directly, rather than C_SRC. Only register
# ones the owning fragment actually enabled; neither hook contains assembly.
KERNEL_HOOK_OBJECTS := $(filter $(BUILD)/tests/unit/usb_storage_guest.o \
  $(BUILD)/tests/unit/pci_intx_guest_kernel.o,$(OBJ))
ifneq ($(strip $(KERNEL_HOOK_OBJECTS)),)
$(eval $(call product_c,kernel-hooks,$(KERNEL_HOOK_OBJECTS),CFLAGS))
endif

# CLI is an installation list, not an object-path authority. These guest
# probes intentionally do not use CLI_RULE. Derive the source directories from
# their owners' inventories and the enabled variant names from the install list.
# Negative controls have distinct object names and fixed per-name defines, so
# their bodies must never be shared with the positive object's compile result.
POLL_PROBE_OBJECTS := $(if $(POLLTEST_SRC),$(addprefix $(BUILD)/pollobj/$(dir $(POLLTEST_SRC)),\
  $(addsuffix .o,$(filter polltest polltest-%,$(CLI)))))
THREAD_PROBE_OBJECTS := $(if $(THRTEST_SRC),$(addprefix $(BUILD)/throbj/$(dir $(THRTEST_SRC)),\
  $(addsuffix .o,$(filter thrtest thrtest-%,$(CLI)))))
SCHED_PROBE_OBJECTS := $(patsubst %.c,$(BUILD)/schedobj/%.o,$(SCHED_SRC))
LIBC_PROBE_OBJECTS := $(patsubst %.c,$(BUILD)/asobj/%.o,$(wildcard $(LIBC2_SRC)))
LIBM_PROBE_OBJECTS := $(addprefix $(BUILD)/libmobj/,$(notdir $(patsubst %.c,%.o,$(wildcard $(LIBMCHK_SRC)))))
DEMUX_PROBE_OBJECTS := $(patsubst %.c,$(BUILD)/medobj/%.o,$(wildcard tests/guest/media/demuxcheck.c))
PRODUCT_PROBE_OBJECTS := $(POLL_PROBE_OBJECTS) $(THREAD_PROBE_OBJECTS) \
  $(SCHED_PROBE_OBJECTS) $(LIBC_PROBE_OBJECTS) $(LIBM_PROBE_OBJECTS) \
  $(DEMUX_PROBE_OBJECTS) $(if $(SECPROBE_SRC),$(BUILD)/apps/secprobe.cli.o)
ifneq ($(strip $(PRODUCT_PROBE_OBJECTS)),)
$(eval $(call product_c,guest-probes,$(PRODUCT_PROBE_OBJECTS),UCFLAGS))
endif
