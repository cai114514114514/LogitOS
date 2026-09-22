# SPDX-License-Identifier: MIT
# Keep the compiler itself available for host links and toolchain inspection.
# Only compilation uses the cache; CCACHE= disables it without changing output.
ifeq ($(origin CCACHE),undefined)
CCACHE := $(shell command -v ccache 2>/dev/null)
endif
CCACHE_DIR ?= $(abspath .cache/ccache)
export CCACHE_DIR
COMPILE = $(if $(strip $(CCACHE)),$(CCACHE) )$(CC)

# This directory deliberately survives make clean and clean-scratch. A fresh
# BUILD must still compile its own dependency files, but ccache can restore
# matching results instead of repeating the compiler's code generation.
