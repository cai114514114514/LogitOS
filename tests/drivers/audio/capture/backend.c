/* External QEMU D-Bus audio peer. Input comes only from a supplied PCM file;
 * no host sound API, microphone, or kernel DMA hook is accessed here. */
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_ROOT "/org/qemu/Display1/Audio"
#define AUDIO_IFACE "org.qemu.Display1.Audio"
#define STREAM_FORMAT \
    "<arg type='t' direction='in'/><arg type='y' direction='in'/>" \
    "<arg type='b' direction='in'/><arg type='b' direction='in'/>" \
    "<arg type='u' direction='in'/><arg type='y' direction='in'/>" \
    "<arg type='u' direction='in'/><arg type='u' direction='in'/>" \
    "<arg type='b' direction='in'/>"
#define COMMON_METHODS \
    "<method name='Init'>" STREAM_FORMAT "</method>" \
    "<method name='Fini'><arg type='t' direction='in'/></method>" \
    "<method name='SetEnabled'><arg type='t' direction='in'/>" \
    "<arg type='b' direction='in'/></method>" \
    "<method name='SetVolume'><arg type='t' direction='in'/>" \
    "<arg type='b' direction='in'/><arg type='ay' direction='in'/></method>" \
    "<property name='Interfaces' type='as' access='read'/>"

struct listener {
    int fd;
    int input;
    unsigned rate;
    unsigned volume[2];
    gboolean muted;
    FILE *source;
    FILE *samples;
    uint64_t transferred;
};

static void fatal(const char *operation, GError *error)
{
    fprintf(stderr, "BACKEND_FAIL %s: %s\n", operation,
            error ? error->message : "invalid input");
    exit(1);
}

static GDBusConnection *connect_peer(int fd)
{
    GError *error = NULL;
    GSocket *socket = g_socket_new_from_fd(fd, &error);
    if (!socket)
        fatal("socket", error);
    GSocketConnection *stream = g_socket_connection_factory_create_connection(socket);
    GDBusConnection *connection = g_dbus_connection_new_sync(G_IO_STREAM(stream), NULL,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING, NULL, NULL, &error);
    if (!connection)
        fatal("peer authentication", error);
    g_object_unref(stream);
    g_object_unref(socket);
    return connection;
}

static GVariant *get_property(GDBusConnection *connection, const gchar *sender,
    const gchar *path, const gchar *interface, const gchar *property,
    GError **error, gpointer opaque)
{
    (void)connection; (void)sender; (void)path; (void)interface;
    (void)property; (void)error; (void)opaque;
    return g_variant_new_strv(NULL, 0);
}

static void method_call(GDBusConnection *connection, const gchar *sender,
    const gchar *path, const gchar *interface, const gchar *method,
    GVariant *parameters, GDBusMethodInvocation *invocation, gpointer opaque)
{
    (void)connection; (void)sender; (void)path; (void)interface;
    struct listener *listener = opaque;
    guint64 id;
    if (!strcmp(method, "Read")) {
        guint64 size;
        g_variant_get(parameters, "(tt)", &id, &size);
        if (!size || size > 65536 || size % 4)
            fatal("invalid record request size", NULL);
        uint8_t *data = g_malloc((gsize)size);
        if (fread(data, 1, (size_t)size, listener->source) != size)
            fatal("synthetic input exhausted", NULL);
        for (size_t index = 0; index < size; index += 2) {
            int16_t original = (int16_t)((unsigned)data[index] | (unsigned)data[index + 1] << 8);
            int sample = listener->muted ? 0 :
                original * (int)listener->volume[(index / 2) % 2] / 255;
            data[index] = (uint8_t)sample;
            data[index + 1] = (uint8_t)((unsigned)sample >> 8);
        }
        if (fwrite(data, 1, (size_t)size, listener->samples) != size)
            fatal("injected PCM evidence write", NULL);
        fflush(listener->samples);
        listener->transferred += size;
        GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, size, 1);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ay)", bytes));
        g_free(data);
        return;
    }
    if (!strcmp(method, "Write")) {
        GVariant *bytes;
        g_variant_get(parameters, "(t@ay)", &id, &bytes);
        gsize size;
        const uint8_t *original = g_variant_get_fixed_array(bytes, &size, 1);
        if (size % 4)
            fatal("partial playback frame", NULL);
        uint8_t *data = g_memdup2(original, size);
        /* D-Bus delegates endpoint volume to its listener. Apply that request
         * before recording what this synthetic output endpoint would play. */
        for (size_t index = 0; index < size; index += 2) {
            int16_t original_sample = (int16_t)((unsigned)data[index] |
                                               (unsigned)data[index + 1] << 8);
            int sample = listener->muted ? 0 : original_sample *
                (int)listener->volume[(index / 2) % 2] / 255;
            data[index] = (uint8_t)sample;
            data[index + 1] = (uint8_t)((unsigned)sample >> 8);
        }
        if (fwrite(data, 1, size, listener->samples) != size)
            fatal("playback PCM evidence write", NULL);
        g_free(data);
        fflush(listener->samples);
        listener->transferred += size;
        g_variant_unref(bytes);
    } else if (!strcmp(method, "Init")) {
        guchar bits, channels;
        gboolean signed_pcm, floating, big_endian;
        guint rate, frame_bytes, second_bytes;
        g_variant_get(parameters, "(tybbuyuub)", &id, &bits, &signed_pcm, &floating,
                      &rate, &channels, &frame_bytes, &second_bytes, &big_endian);
        if (bits != 16 || channels != 2 || !signed_pcm || floating || big_endian ||
            rate != listener->rate || frame_bytes != 4 || second_bytes != rate * 4)
            fatal("unexpected native stream format", NULL);
    } else if (!strcmp(method, "SetVolume")) {
        GVariant *volumes;
        g_variant_get(parameters, "(tb@ay)", &id, &listener->muted, &volumes);
        gsize channels;
        const guint8 *values = g_variant_get_fixed_array(volumes, &channels, 1);
        if (channels != 2)
            fatal("unexpected volume channel count", NULL);
        listener->volume[0] = values[0];
        listener->volume[1] = values[1];
        g_variant_unref(volumes);
    }
    if (strcmp(method, "Write")) {
        gchar *description = g_variant_print(parameters, FALSE);
        printf("BACKEND_%s %s bytes=%llu %s\n", listener->input ? "IN" : "OUT",
               method, (unsigned long long)listener->transferred, description);
        fflush(stdout);
        g_free(description);
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static gint closed_listeners;

static void listener_closed(GDBusConnection *connection, gboolean remote_vanished,
                            GError *error, gpointer opaque)
{
    (void)connection; (void)remote_vanished; (void)error;
    struct listener *listener = opaque;
    printf("BACKEND_CLOSED %s\n", listener->input ? "IN" : "OUT");
    fflush(stdout);
    /* Both listener connections have drained their queued method callbacks.
     * Exit flushes the evidence files before the parent computes their hashes. */
    if (g_atomic_int_add(&closed_listeners, 1) == 1)
        exit(0);
}

static gpointer listener_thread(gpointer opaque)
{
    struct listener *listener = opaque;
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GDBusConnection *connection = connect_peer(listener->fd);
    const char *direction = listener->input ? "In" : "Out";
    gchar *xml = g_strdup_printf("<node><interface name='org.qemu.Display1.Audio%sListener'>"
        COMMON_METHODS "%s</interface></node>", direction,
        listener->input ? "<method name='Read'><arg type='t' direction='in'/>"
        "<arg type='t' direction='in'/><arg type='ay' direction='out'/></method>" :
        "<method name='Write'><arg type='t' direction='in'/>"
        "<arg type='ay' direction='in'/></method>");
    GError *error = NULL;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, &error);
    if (!info)
        fatal("listener introspection", error);
    gchar *path = g_strdup_printf("/org/qemu/Display1/Audio%sListener", direction);
    static const GDBusInterfaceVTable methods = {.method_call = method_call,
                                                .get_property = get_property};
    if (!g_dbus_connection_register_object(connection, path, info->interfaces[0],
                                          &methods, listener, NULL, &error))
        fatal("listener export", error);
    g_signal_connect(connection, "closed", G_CALLBACK(listener_closed), listener);
    g_dbus_connection_start_message_processing(connection);
    GMainLoop *loop = g_main_loop_new(context, FALSE);
    g_main_loop_run(loop);
    return NULL;
}

static void register_listener(GDBusConnection *connection, struct listener *listener)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
        fatal("listener socket pair", NULL);
    listener->fd = pair[0];
    g_thread_new(listener->input ? "input" : "output", listener_thread, listener);
    GError *error = NULL;
    GUnixFDList *fds = g_unix_fd_list_new();
    int handle = g_unix_fd_list_append(fds, pair[1], &error);
    if (handle < 0)
        fatal("listener fd export", error);
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(connection, NULL,
        AUDIO_ROOT, AUDIO_IFACE, listener->input ? "RegisterInListener" : "RegisterOutListener",
        g_variant_new("(h)", handle), NULL, G_DBUS_CALL_FLAGS_NONE, 10000, fds,
        NULL, NULL, &error);
    if (!reply)
        fatal("listener registration", error);
    g_variant_unref(reply);
    g_object_unref(fds);
    close(pair[1]);
}

int main(int argc, char **argv)
{
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "usage: backend <peer-fd> <input-rate> <source.pcm> <injected.pcm> <output.pcm> [output-rate]\n");
        return 2;
    }
    GDBusConnection *connection = connect_peer(atoi(argv[1]));
    g_dbus_connection_start_message_processing(connection);
    struct listener input = {.input = 1, .rate = (unsigned)strtoul(argv[2], NULL, 10),
                             .volume = {255, 255}};
    /* Separate controllers need not share a clock. Validating both listeners
     * against the ADC rate would silently demand output resampling from QEMU
     * and would no longer test the playback driver's native PCM trajectory. */
    struct listener output = {.rate = argc == 7 ? (unsigned)strtoul(argv[6], NULL, 10)
                                              : input.rate,
                              .volume = {255, 255}};
    input.source = fopen(argv[3], "rb");
    input.samples = fopen(argv[4], "wb");
    output.samples = fopen(argv[5], "wb");
    if (!input.source || !input.samples || !output.samples)
        fatal("PCM file open", NULL);
    register_listener(connection, &input);
    register_listener(connection, &output);
    puts("BACKEND_READY");
    fflush(stdout);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
