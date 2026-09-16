# Compiler source ownership. Runtime units are linked into generated programs,
# not into the compiler; a recursive wildcard over the whole tree mixes these
# two different executables. Keep that boundary explicit here.
AS_NATIVE_DIRS := cli common frontend sema backend/llvm
# `legacy` was here until the A2 engine was deleted. AS_COMPILER_DIRS and
# AS_NATIVE_DIRS now differ by exactly `editor`, which is the Studio completion
# surface rather than a second engine -- so the distinction the two variables
# encode is finally the one their names claim.
AS_COMPILER_DIRS := $(AS_NATIVE_DIRS) editor
AS_C := $(sort $(foreach dir,$(AS_COMPILER_DIRS),$(wildcard c/apps/as/$(dir)/*.c)))
AS_HDRS := $(sort $(foreach dir,$(AS_COMPILER_DIRS) include ir runtime,$(wildcard c/apps/as/$(dir)/*.h)))

# The native-only harness has its own main and must never acquire a VM dependency.
AS_NATIVE_FRONTEND := $(filter-out c/apps/as/cli/main.c,$(foreach dir,$(AS_NATIVE_DIRS),$(wildcard c/apps/as/$(dir)/*.c)))
AS_HOST_SOURCES := $(filter-out c/apps/as/editor/completion.c,$(AS_C))
AS_CORE := $(filter-out c/apps/as/cli/main.c,$(AS_HOST_SOURCES))
