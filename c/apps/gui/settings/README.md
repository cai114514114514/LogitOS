# Settings

Eight native pages: Appearance, Desktop, Network, Dock, Mouse & scrolling,
Date & time, All settings, and System. The editor exposes 21 preferences.
System reports live display geometry, memory and the current network address.

`settings.c` owns the event loop and draft lifecycle. `model.h` validates
network input and writes only edited fields. `store.inc` reads the preference
inspector; `colour.inc` owns colour conversion. `view.inc` and `desktop.inc`
lay out the pages; `preview.inc` records and caches OpenLogit/GFX surfaces.

Desktop preference keys, ranges and defaults have one authority:
`include/abi/logit_desktop_prefs.h`. The schema, desktop consumer and editor
expand that list. Dock, input and clock changes apply within the existing
half-second desktop wake. Browser restore applies at the next Browser launch;
wallpaper and network changes apply after restart.

Edits stay local until Apply. Revert reads the live store without reloading the
disk or discarding another process's in-memory work. Dark appearance is an
immediate system action and is labelled accordingly. The generation check
detects intervening committed changes, but the ABI has no compare-and-swap or
multi-key live transaction: a failed save can leave earlier writes active in
RAM. The application reports that failure and retains the draft.

The original generated PNG is `fsroot/icons/settings.png`. The kernel's bounded
raster-icon cache consumes `/icons/<executable stem>.png`; Dock, Finder and the
Settings title use it. Missing artwork keeps the existing fallback. Assets are
packaged by `tests/settings2.mk`, including when using the ordinary `make run`.
The bitmap icon is separate from the UI design reference in
`docs/design/settings/concept.png`.

Build and verify:

```sh
make BUILD=build-settings build-settings/logit.iso build-settings/disk.img
make BUILD=build-settings test-settings-draft test-app-icons test-settings-host test-mk-wired
make BUILD=build-settings test-settings-ui
```

The UI gate boots QEMU, drives all eight pages, compares light/dark desktop
pixels, validates input, saves a preference, filters the inspector, checks the
raster Dock icon, changes the actual menu-bar clock, and cancels an unsaved
close. The draft and raster-loader gates have mandatory failing controls.
