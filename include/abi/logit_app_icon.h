/* SPDX-License-Identifier: MIT */
#ifndef LOGIT_APP_ICON_H
#define LOGIT_APP_ICON_H
/* SYS_GUI_ICON: with this reserved id, arg c is a NUL-terminated executable
 * name instead of a tint. Returns 0 when a raster asset was drawn, -1 when
 * absent, so existing vector/letter fallbacks stay usable on older disks. */
#define LOGIT_ICON_APP 65535
#define LOGIT_ICON_NAME_MAX 48
#endif
