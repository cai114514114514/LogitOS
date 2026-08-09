/* A three-type stand-in for <glib.h>, so tools/mmtrace/ can build against
 * QEMU's plugin header on a machine with no libglib2.0-dev installed.
 *
 * qemu-plugin.h touches exactly two glib types -- GArray (the register list
 * from qemu_plugin_get_registers) and GByteArray (the buffer
 * qemu_plugin_read_register fills). Both are public, stable, ABI-frozen
 * structs whose first two members are the only ones the plugin ever reads.
 * The FUNCTIONS are not defined here and are not linked: a plugin is dlopen'd
 * into a QEMU that already links glib, so g_byte_array_new() and friends
 * resolve out of the host process at load time. That is why the declarations
 * below are declarations and there is no -lglib-2.0 anywhere.
 *
 * If libglib2.0-dev is ever installed, tests/mmtrace.mk prefers the real
 * headers (it only adds -Itools/mmtrace/stub when glib.h is missing), so this
 * file goes quietly unused rather than shadowing the real thing. */
#ifndef LOGIT_MMTRACE_GLIB_STUB_H
#define LOGIT_MMTRACE_GLIB_STUB_H

#include <stdint.h>
#include <stddef.h>

typedef char           gchar;
typedef int            gint;
typedef unsigned int   guint;
typedef unsigned char  guint8;
typedef int            gboolean;
typedef void          *gpointer;
typedef const void    *gconstpointer;

/* Layout as in glib/garray.h. Only ->data and ->len are public and only those
 * two are used here; the opaque tail is not declared because nothing may touch
 * it -- these are always allocated by glib itself, never by us. */
typedef struct _GArray {
    gchar *data;
    guint  len;
} GArray;

typedef struct _GByteArray {
    guint8 *data;
    guint   len;
} GByteArray;

#define g_array_index(a, t, i) (((t *)(void *)(a)->data)[(i)])

/* Resolved from the QEMU process at dlopen time. */
extern GByteArray *g_byte_array_new(void);
extern GByteArray *g_byte_array_set_size(GByteArray *a, guint len);
extern guint8     *g_byte_array_free(GByteArray *a, gboolean free_segment);
extern gchar      *g_array_free(GArray *a, gboolean free_segment);
extern void        g_free(gpointer p);

#endif /* LOGIT_MMTRACE_GLIB_STUB_H */
