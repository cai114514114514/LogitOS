/* On-demand native event provenance, NOT a focus/interaction workaround.
 * A real textarea was painted and QEMU's pointer reached its rectangle, yet
 * typing left the placeholder untouched. Those two facts cannot distinguish
 * an overlay hit, cancelled mousedown, lost focus or cancelled key default.
 * about:input arms 64 trace points through the actual dispatcher; ordinary
 * browsing stays silent. Never log key values, field contents or password
 * lengths, and never invoke a page callback merely to inspect its state. */
static unsigned g_input_trace_budget;
static int g_input_trace_needs_frame;
static void input_trace(const char *phase, struct node *hit, int x, int y, int allowed, int address_editing)
{
    if (!g_input_trace_budget) return;
    g_input_trace_budget--;
    struct node *f = focus_current();
    const char *id = hit ? dom_attr(hit, "id") : 0;
    const char *cls = hit ? dom_attr(hit, "class") : 0;
    const char *fid = f ? dom_attr(f, "id") : 0;
    int len = -1, kind = fc_kind(f);
    if (FC_IS_TEXTUAL(kind) && kind != FC_PASSWORD) fc_value(f, &len);
    printf("[input-trace] %s point=%d,%d allowed=%d editing=%d hit=<%s>#%.48s .%.96s focus=<%s>#%.48s kind=%d disabled=%d readonly=%d length=%d remaining=%u guest_ms=%llu\n",
        phase,x,y,allowed,address_editing,hit?hit->tag:"-",id?id:"",cls?cls:"",
        f?f->tag:"-",fid?fid:"",kind,f?fc_disabled(f):0,f?fc_readonly(f):0,
        len,g_input_trace_budget,(unsigned long long)monotonic_ms());
}
