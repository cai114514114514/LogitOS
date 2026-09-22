# Archived validation evidence — 2026-09-22

The owner requested categorized commits on main and removal of build directories.
These existing logs, results and referenced screenshots were copied before cleanup;
they were not regenerated or reclassified as new successful checks. `manifest.json`
records their original build paths and SHA-256 digests.

Historical reproduction commands in the reports still use `build-*` output paths.
The files preserved here are evidence, not build inputs. Disk images, ISO files,
object files and temporary binaries are deliberately excluded. Rebuild those from
main when needed; a later build is not byte-identical evidence for an older run.

The reported intermittent guest exit stall and unsupported-host SKIPs remain open.
Deleting temporary build artifacts does not turn them into passing results.
