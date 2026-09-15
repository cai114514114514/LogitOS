# Interaction batch paused

Paused at the owner's request while the parent agent investigates the reported
chat.deepseek.com browser exit. No files were reverted.

Prepared files:

- tests/fixtures/engine-interaction/index.html: ordinary hover menu, pressed text
  color, Tab focus targets, inert subtree, and click-to-focus action. No internal
  JS probes; this same HTML can be used for guest interaction.
- tests/unit/interaction_runtime_test.c: real app_main plus existing loader host
  event and paint recorders. Observes menu visibility, active/focus text colors,
  activeElement, inert click outcome, and orderly window exit. Each event lets
  the real frame loop repaint before the next observation.
- tests/interaction_runtime.mk: derived shipping loader/CSSOM sources plus real
  js_forms.c. Tracks textual .inc/.h and queue/storage implementation inputs.
  test-interaction-runtime-negctl is a positive-target prerequisite and requires
  visible hover/active/Tab-focus assertion failures with
  CSS_NEGCTL_STATIC_INTERACTION.

Build started with BUILD=build-interaction-wave2, then was stopped on instruction
using Ctrl-C (exit 130). No test executable has been run and no result is claimed.
Build log: /tmp/interaction-runtime-build.log.

Resume after native CSS/inert changes are ready. Confirm source list and make
fragment wiring with the parent (this agent did not edit Makefile), compile,
run the normal fixture, diagnose recorder issues before product failures, then
watch the negative-control assertions fail. Report host-only evidence; guest
verification remains with the parent.
