# SPDX-License-Identifier: MIT
# All owned program targets retain their source/codec/CRT lists. Only their
# final link is augmented; browser and third-party outputs never enter this list.
AGENT_AR ?= $(shell command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)
AGENT_DIR := $(BUILD)/agent
AGENT_REAL_LD := $(LD)
AGENT_NAMES := $(shell python3 tools/agent_catalog.py --names)
AGENT_TARGETS := $(addprefix $(BUILD)/,$(addsuffix .elf,$(AGENT_NAMES)))
AGENT_SDK_SRC := $(filter-out c/lib/agent/model.c c/lib/agent/model_config.c,$(wildcard c/lib/agent/*.c)) c/drivers/block/crc32.c c/crypto/hash/sha256.c
AGENT_SDK_OBJ := $(patsubst %.c,$(AGENT_DIR)/%.o,$(AGENT_SDK_SRC))
AGENT_LIBC_OBJ := $(patsubst %.c,$(AGENT_DIR)/%.o,$(wildcard c/apps/libc/src/*.c)) $(patsubst %.asm,$(AGENT_DIR)/%.o,$(wildcard c/apps/libc/src/*.asm))
$(AGENT_DIR)/agent_catalog.inc: tools/agent_catalog.py c/apps/agent/catalog.json
	@python3 tools/agent_catalog.py --header $@
$(AGENT_DIR)/%.o: %.c $(AGENT_DIR)/agent_catalog.inc
	@mkdir -p $(dir $@)
	$(COMPILE) $(UCFLAGS) -I$(AGENT_DIR) -ffunction-sections -fdata-sections -c $< -o $@
$(AGENT_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@
$(AGENT_DIR)/entry.o: c/apps/crt0_agent.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@
$(AGENT_DIR)/c/apps/agent/agentd.o: UCFLAGS += $(AGENT_BROKER_FLAGS)
$(AGENT_DIR)/sdk.a: $(AGENT_SDK_OBJ) tools/agent_archive.py
	python3 tools/agent_archive.py $(AGENT_AR) $@ $(AGENT_SDK_OBJ)
$(AGENT_DIR)/libc.a: $(AGENT_LIBC_OBJ) tools/agent_archive.py
	python3 tools/agent_archive.py $(AGENT_AR) $@ $(AGENT_LIBC_OBJ)
$(AGENT_TARGETS): $(AGENT_DIR)/entry.o $(AGENT_DIR)/sdk.a $(AGENT_DIR)/libc.a tools/agent_link.py
$(AGENT_TARGETS): LD = python3 tools/agent_link.py $(BUILD) $(AGENT_REAL_LD)

$(BUILD)/assistant.elf: c/apps/gui/assistant/assistant.c $(AGENT_DIR)/c/apps/gui/assistant/assistant.o $(AUI_OBJ) $(GFX_OBJ) c/apps/crt0_cli.asm
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(AGENT_DIR)/assistant-crt.o
	$(LD) -nostdlib -e _start -Ttext=0x4D000000 -o $@ $(AGENT_DIR)/assistant-crt.o $(AGENT_DIR)/c/apps/gui/assistant/assistant.o $(AUI_OBJ) $(GFX_OBJ)
$(BUILD)/assistant.aex: $(BUILD)/assistant.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ Tasks - A 80 160 240 --gui
$(BUILD)/agentd.elf: $(AGENT_DIR)/c/apps/agent/agentd.o $(AGENT_DIR)/c/apps/agent/userfs.o $(AGENT_DIR)/c/lib/agent/model.o $(AGENT_DIR)/c/lib/agent/model_config.o $(AGENT_DIR)/c/net/http/http1.o c/apps/crt0_cli.asm
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(AGENT_DIR)/agentd-crt.o
	$(LD) -nostdlib --gc-sections -e _start -Ttext=0x50000000 -o $@ $(AGENT_DIR)/agentd-crt.o $(AGENT_DIR)/c/apps/agent/agentd.o $(AGENT_DIR)/c/apps/agent/userfs.o $(AGENT_DIR)/c/lib/agent/model.o $(AGENT_DIR)/c/lib/agent/model_config.o $(AGENT_DIR)/c/net/http/http1.o
$(BUILD)/agentd.aex: $(BUILD)/agentd.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ agentd - A 80 160 240 --cli

# The ordinary disk recipe expands ROOT_AEX_PACK at use time.
# fsroot/etc is already packed by FS_FILES; adding agent.conf twice is refused.
ROOT_AEX_PACK += $(BUILD)/assistant.aex:assistant.aex $(BUILD)/agentd.aex:/bin/agentd
$(DISK): $(BUILD)/assistant.aex $(BUILD)/agentd.aex fsroot/etc/agent.conf


$(AGENT_DIR)/link-contract: tools/agent_link.py tests/agent.mk
	@mkdir -p $(dir $@)
	@touch $@
$(AGENT_TARGETS): $(AGENT_DIR)/link-contract
-include $(AGENT_SDK_OBJ:.o=.d) $(AGENT_LIBC_OBJ:.o=.d)
-include $(AGENT_DIR)/c/apps/agent/agentd.d $(AGENT_DIR)/c/lib/agent/model.d $(AGENT_DIR)/c/apps/gui/assistant/assistant.d
-include $(AGENT_DIR)/c/apps/agent/userfs.d
-include $(AGENT_DIR)/c/lib/agent/model_config.d

$(eval $(call CLI_RULE,agentctl))
ROOT_AEX_PACK += $(BUILD)/agentctl.aex:/bin/agentctl
$(DISK): $(BUILD)/agentctl.aex

# These owned diagnostics existed as sources without normal package targets.
# They now use the same authenticated activation bridge as other CLI tools.
AGENT_EXTRA_CLI := crash free ps readcore uptime
$(foreach c,$(AGENT_EXTRA_CLI),$(eval $(call CLI_RULE,$(c))))
ROOT_AEX_PACK += $(foreach c,$(AGENT_EXTRA_CLI),$(BUILD)/$(c).aex:/bin/$(c))
$(DISK): $(addprefix $(BUILD)/,$(addsuffix .aex,$(AGENT_EXTRA_CLI)))

$(BUILD)/agent-runtime-test.elf: tests/fixtures/agent/runtime.c $(AGENT_DIR)/c/apps/agent/userfs.o $(AGENT_DIR)/sdk.a $(AGENT_DIR)/libc.a c/apps/crt0_cli.asm
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(AGENT_DIR)/runtime-crt.o
	$(CC) $(UCFLAGS) -c $< -o $(AGENT_DIR)/runtime.o
	$(AGENT_REAL_LD) -nostdlib --gc-sections -e _start -Ttext=0x50000000 -o $@ $(AGENT_DIR)/runtime-crt.o $(AGENT_DIR)/runtime.o $(AGENT_DIR)/c/apps/agent/userfs.o --start-group $(AGENT_DIR)/sdk.a $(AGENT_DIR)/libc.a --end-group
$(BUILD)/agent-runtime-test.aex: $(BUILD)/agent-runtime-test.elf tests/fixtures/agent/runtime.json tools/mkaex.py
	python3 tools/mkaex.py $< $@ AgentRuntime - A 80 160 240 --cli --id os.logit.agent-runtime-test --agent tests/fixtures/agent/runtime.json


# The Python runner also owns publication fault builds, its host gateway and
# the six real-model guests. Only its private ephemeral key enters test disks.
AGENT_ACCEPT_OUT ?= $(BUILD)/agent-acceptance
AGENT_ENV ?= .env

# Parsing/file-read failures are tested without linking socket/HTTP transport.
# Former parser controls must fail their named semantic assertions first.


# Exercise the normal CLI path from a frozen catalog build. The runner copies
# artifacts before boot; this gate never rebuilds or writes the source BUILD.
AGENT_CLI_BUILD ?= $(BUILD)
AGENT_CLI_OUT ?= $(BUILD)/agent-cli
AGENT_CLI_MODE ?= bios
AGENT_CLI_RAM ?= 512M

# Quiet-window evidence is independent of model traffic. The runner first
# measures status RPC overhead, then saves raw and corrected guest counters.
AGENT_IDLE_BUILD ?= $(BUILD)
AGENT_IDLE_OUT ?= $(BUILD)/agent-idle
AGENT_IDLE_MODE ?= bios
AGENT_IDLE_RAM ?= 512M

# A prepared build is frozen by the caller, as with the CLI/idle guest gates.
# The control restores the exact formerly fatal branch in a private build.
AGENT_STARTUP_OUT ?= $(BUILD)/agent-startup

# Launch the user's existing disk without a system repack: a document saved at
# /untitled.txt is outside that packer's preservation roots. Only agentd and
# the session gateway configuration changed in the original launcher. The
# document workspace now also installs matching TextEdit, Tasks and agentctl; all other paths
# (including user documents at the filesystem root) still survive verbatim.
AGENT_SESSION_DISK ?= $(DISK)
AGENT_SESSION_LIMIT ?= 32
AGENT_QEMU_NET ?= -netdev user,id=n0 -device e1000,netdev=n0
AGENT_SESSION_LAUNCH = python3 tools/agent_session.py --broker $(BUILD)/agentd.aex --finder $(BUILD)/files.aex --textedit $(BUILD)/textedit.aex --assistant $(BUILD)/assistant.aex --agentctl $(BUILD)/agentctl.aex --snapshot-helper $(BUILD)/lfs_snapshot --env $(AGENT_ENV) --state-dir $(BUILD)/agent-sessions --limit $(AGENT_SESSION_LIMIT)
.PHONY: run-agent
run run-agent: $(BUILD)/agentd.aex $(BUILD)/agentctl.aex $(BUILD)/files.aex $(BUILD)/assistant.aex $(BUILD)/textedit.aex $(BUILD)/lfs_snapshot
run-agent: $(ISO)
	$(AGENT_SESSION_LAUNCH) --disk $(AGENT_SESSION_DISK) -- $(QEMU) -cdrom $(ISO) -drive file=$(AGENT_SESSION_DISK),format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 -boot d $(QEMU_RAM) $(QEMU_SMP) $(QEMU_CPU) $(QEMU_RTC) $(QEMU_GPU) $(AGENT_QEMU_NET) $(QEMU_DISP) $(QEMU_SND) -serial stdio -no-reboot -qmp unix:/tmp/logit-qmp.sock,server,nowait


AGENT_SESSION_OUT ?= $(BUILD)/agent-session-acceptance

# TextEdit's native workspace is kept local to its translation unit; changes
# to the layout or bounded editor must rebuild the ordinary app as well.
$(BUILD)/textedit.elf: c/apps/gui/textedit/textedit_work.inc c/apps/gui/textedit/textedit_document.h

TEXTEDIT_WORK_OUT ?= $(BUILD)/textedit-work
TEXTEDIT_WORK_GATEWAY ?=


# Project identities are tested through the real filesystem and native apps.
$(BUILD)/files.elf: c/apps/gui/files/files_project.inc
$(AGENT_DIR)/c/apps/agent/agentd.o: c/apps/agent/project_bindings.inc
PROJECT_OUT ?= $(BUILD)/project-acceptance

PROJECT_SOURCE_DISK ?= build/disk.img
PROJECT_DISK ?= $(BUILD)/project.img
.PHONY: run-project
run-project: $(ISO) $(BUILD)/lfs_snapshot $(addprefix $(BUILD)/,agentd.aex agentctl.aex files.aex textedit.aex assistant.aex)
	python3 tools/project_session.py --source $(PROJECT_SOURCE_DISK) --disk $(PROJECT_DISK) --build $(BUILD) --env $(AGENT_ENV) --limit $(AGENT_SESSION_LIMIT) -- $(QEMU) -cdrom $(ISO) -drive file=$(PROJECT_DISK),format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 -boot d $(QEMU_RAM) $(QEMU_SMP) $(QEMU_CPU) $(QEMU_RTC) $(QEMU_GPU) $(AGENT_QEMU_NET) $(QEMU_DISP) $(QEMU_SND) -serial stdio -no-reboot
PROJECT_GUEST_BINS := agentd agentctl files textedit assistant agent-runtime-test login sh cat echo mv mkdir rm cp


DEFAULT_SESSION_OUT ?= $(BUILD)/default-session-acceptance
