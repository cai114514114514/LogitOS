/* Host-side gate for the HDA CAPTURE widget-graph search.
 *
 * WHAT THIS DOES AND DOES NOT PROVE, stated up front because it would be easy
 * to misread this as "hda.c was tested" -- it was not, and here is exactly
 * why not. c/drivers/audio/hda.c's find_capture_path()/try_capture_path()
 * talk to the codec through codec_cmd(), which pokes real MMIO registers
 * (CORB/RIRB rings) and spins on a hardware handshake; there is no host
 * build of that path the way c/kernel/audio/pcm.c has one (pcm.c is "free of
 * kernel dependencies on purpose... compiled unchanged into
 * tests/unit/audio_pcm_test.c" -- see snd.h). Reaching the exact same
 * translation unit from the host would mean faking a CORB/RIRB command bus,
 * which is a project in itself and not what this gate is for.
 *
 * So what follows is a PORT: the exact same algorithm as
 * try_capture_path()/hda_conn_len()/hda_conn_entry()/nid_in() in hda.c,
 * copied structurally (same shape, same variable names, same two-hop
 * search, same refusal-over-guessing rule for capture), rewritten against a
 * plain in-memory widget table instead of codec_cmd(). It is the shape the
 * task that produced this file asked for explicitly: "gate what you CAN:
 * the codec graph search and the stream setup, host-side, against a
 * modelled widget graph." A port can drift from the original silently --
 * exactly the risk this tree's AetherScript self-hosting-tax section warns
 * about for a different reason -- so this file is deliberately small and its
 * header names the risk instead of hiding it. Anyone changing
 * try_capture_path() in hda.c should re-check this file by eye against it.
 *
 * Build: cc -O2 -Wall -Wextra -o hda_capture_graph_test \
 *          tests/unit/hda_capture_graph_test.c
 * (No project headers needed -- this file is entirely self-contained,
 * mirroring pcm.c's "no kernel dependencies at all" posture on purpose.)
 */
#include <stdio.h>
#include <string.h>

/* ---- the widget types, copied from hda.c's #defines (same values) -------- */
#define WIDGET_DAC   0x0
#define WIDGET_ADC   0x1
#define WIDGET_MIXER 0x2
#define WIDGET_SEL   0x3
#define WIDGET_PIN   0x4

#define MAX_W 32
#define MAX_CONN 8

struct widget {
    unsigned nid;
    unsigned type;
    unsigned pincap;          /* PIN only: bit4 out-capable, bit5 in-capable */
    unsigned port_conn;       /* PIN only: cfg bits 31:30 -- 0x1 = "no physical connection" */
    unsigned conn[MAX_CONN];
    unsigned nconn;
};

struct graph {
    struct widget w[MAX_W];
    unsigned n;
};

static struct widget *find_w(struct graph *g, unsigned nid)
{
    for (unsigned i = 0; i < g->n; i++) if (g->w[i].nid == nid) return &g->w[i];
    return NULL;
}

static void add_widget(struct graph *g, unsigned nid, unsigned type)
{
    struct widget *w = &g->w[g->n++];
    memset(w, 0, sizeof *w);
    w->nid = nid; w->type = type;
}

static void add_pin(struct graph *g, unsigned nid, unsigned pincap, unsigned port_conn)
{
    add_widget(g, nid, WIDGET_PIN);
    g->w[g->n - 1].pincap = pincap;
    g->w[g->n - 1].port_conn = port_conn;
}

static void wire(struct graph *g, unsigned nid, unsigned src)
{
    struct widget *w = find_w(g, nid);
    w->conn[w->nconn++] = src;
}

/* ---- mirrors hda_conn_len / hda_conn_entry / nid_in ----------------------- */
static int mock_conn_len(struct graph *g, unsigned nid)
{
    struct widget *w = find_w(g, nid);
    int n = w ? (int)w->nconn : 0;
    return n > 8 ? 8 : n;
}
static int mock_conn_entry(struct graph *g, unsigned nid, unsigned idx, unsigned *out)
{
    struct widget *w = find_w(g, nid);
    if (!w || idx >= w->nconn) return -1;
    *out = w->conn[idx];
    return 0;
}
static int nid_in(unsigned nid, const unsigned *arr, unsigned n)
{
    for (unsigned i = 0; i < n; i++) if (arr[i] == nid) return 1;
    return 0;
}
static unsigned widget_type(struct graph *g, unsigned nid)
{
    struct widget *w = find_w(g, nid);
    return w ? w->type : 0xFFFFFFFFu;
}

/* Result struct, mirroring the fields try_capture_path() writes on h. */
struct result {
    unsigned adc_nid, cap_pin_nid, cap_sel_nid, cap_sel_index;
    int found;
};

/* Direct PORT of try_capture_path() in hda.c -- same control flow, same
 * refuse-rather-than-guess rule, same two-hop depth. */
static int try_capture_path(struct graph *g, const unsigned *adcs, unsigned nadc,
                            const unsigned *inpins, unsigned ninpin, struct result *r)
{
    memset(r, 0, sizeof *r);
    if (!nadc || !ninpin) return 0;

    for (unsigned a = 0; a < nadc; a++) {
        int len = mock_conn_len(g, adcs[a]);
        for (unsigned k = 0; k < (unsigned)len; k++) {
            unsigned entry, t;
            if (mock_conn_entry(g, adcs[a], k, &entry) != 0) break;
            t = widget_type(g, entry);
            if (t == WIDGET_PIN && nid_in(entry, inpins, ninpin)) {
                r->adc_nid = adcs[a];
                r->cap_pin_nid = entry;
                if (len > 1) { r->cap_sel_nid = adcs[a]; r->cap_sel_index = k; }
                r->found = 1;
                return 1;
            }
            if (t == WIDGET_SEL || t == WIDGET_MIXER) {
                int len2 = mock_conn_len(g, entry);
                for (unsigned k2 = 0; k2 < (unsigned)len2; k2++) {
                    unsigned e2;
                    if (mock_conn_entry(g, entry, k2, &e2) != 0) break;
                    if (nid_in(e2, inpins, ninpin)) {
                        r->adc_nid = adcs[a];
                        r->cap_pin_nid = e2;
                        if (t == WIDGET_SEL && len2 > 1) {
                            r->cap_sel_nid = entry; r->cap_sel_index = k2;
                        } else if (len > 1) {
                            r->cap_sel_nid = adcs[a]; r->cap_sel_index = k;
                        }
                        r->found = 1;
                        return 1;
                    }
                }
            }
        }
    }
#ifdef HDA_CAP_NEGCTL_GUESS
    /* THE NEGATIVE CONTROL: the exact wrong implementation the task this
     * file gates against named by name -- "a pin that is not... connected
     * must be REFUSED rather than configured into silence" -- reinstated
     * here as the plausible one-liner a reviewer might otherwise reach for
     * (it is, after all, exactly what find_output_path does on the OUTPUT
     * side). Wired to a `make` flag rather than left as dead code so it is
     * provably reachable: `make test-hda-capture-graph-negctl` below. */
    if (nadc && ninpin) {
        r->adc_nid = adcs[0];
        r->cap_pin_nid = inpins[0];
        r->found = 1;
        return 1;
    }
#endif
    return 0;   /* refused: no fallback, unlike the output side */
}

/* PORT of find_output_path()'s widget-gathering loop, the PIN branch only --
 * same connectivity + capability gate hda.c uses (pincap bit5 = input
 * capable, port_conn == 0x1 = "no physical connection", refused). */
static void gather(struct graph *g, unsigned *adcs, unsigned *nadc,
                   unsigned *inpins, unsigned *ninpin)
{
    *nadc = 0; *ninpin = 0;
    for (unsigned i = 0; i < g->n; i++) {
        struct widget *w = &g->w[i];
        if (w->type == WIDGET_ADC) {
            adcs[(*nadc)++] = w->nid;
        } else if (w->type == WIDGET_PIN) {
            int connected = w->port_conn != 0x1;
            if (connected && (w->pincap & (1u << 5))) inpins[(*ninpin)++] = w->nid;
        }
    }
}

/* --------------------------------------------------------------- harness -- */
static int g_fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else printf("ok   %s\n", msg); \
} while (0)

/* Case 1: QEMU's shape -- one ADC whose connection list names the pin
 * directly, one hop, no selector needed (len == 1, so cap_sel_nid stays 0 --
 * matching hda.c's "if (len > 1)" guard: a single-source ADC needs no
 * SET_CONNECT_SELECT at all). */
static void case_direct(void)
{
    struct graph g = {0};
    add_widget(&g, 2, WIDGET_DAC);
    add_pin(&g, 3, (1u << 4), 0x0);              /* output pin, connected */
    add_widget(&g, 4, WIDGET_ADC);
    add_pin(&g, 5, (1u << 5), 0x0);               /* input pin, connected */
    wire(&g, 4, 5);                                /* ADC's sole source is the pin */

    unsigned adcs[8], nadc, inpins[8], ninpin;
    gather(&g, adcs, &nadc, inpins, &ninpin);
    CHECK(nadc == 1 && adcs[0] == 4, "direct: one ADC gathered (nid 4)");
    CHECK(ninpin == 1 && inpins[0] == 5, "direct: one input pin gathered (nid 5)");

    struct result r;
    int ok = try_capture_path(&g, adcs, nadc, inpins, ninpin, &r);
    CHECK(ok == 1, "direct: capture path found");
    CHECK(r.adc_nid == 4, "direct: adc_nid == 4");
    CHECK(r.cap_pin_nid == 5, "direct: cap_pin_nid == 5");
    CHECK(r.cap_sel_nid == 0, "direct: no select needed (single source)");
}

/* Case 2: pin -> input selector (two candidate sources) -> ADC. The
 * selector's OWN index must be recorded (cap_sel_nid == selector, not the
 * ADC), because a mixer sums and a selector chooses -- and picking the wrong
 * one of those two widgets to program is exactly the bug class
 * codec_setup_capture's comment warns about (gain-staging or selecting the
 * wrong source). */
static void case_selector(void)
{
    struct graph g = {0};
    add_widget(&g, 10, WIDGET_ADC);
    add_widget(&g, 11, WIDGET_SEL);
    add_pin(&g, 12, (1u << 5), 0x0);   /* mic, index 0 on the selector */
    add_pin(&g, 13, (1u << 5), 0x0);   /* line-in, index 1 on the selector */
    wire(&g, 11, 12);
    wire(&g, 11, 13);
    wire(&g, 10, 11);                  /* ADC's sole source is the selector */

    unsigned adcs[8], nadc, inpins[8], ninpin;
    gather(&g, adcs, &nadc, inpins, &ninpin);
    CHECK(ninpin == 2, "selector: two input pins gathered");

    struct result r;
    int ok = try_capture_path(&g, adcs, nadc, inpins, ninpin, &r);
    CHECK(ok == 1, "selector: capture path found");
    CHECK(r.adc_nid == 10, "selector: adc_nid == 10");
    CHECK(r.cap_pin_nid == 12, "selector: resolves to the FIRST reachable pin (mic, index 0)");
    CHECK(r.cap_sel_nid == 11 && r.cap_sel_index == 0,
          "selector: select is programmed on the SELECTOR (11), index 0 -- not the ADC");
}

/* Case 3: pin -> summing mixer -> ADC. A mixer has no select verb (it sums
 * every connected source), so cap_sel_nid must stay 0 even though the mixer
 * itself has more than one input -- getting this backwards would send a
 * SET_CONNECT_SELECT verb to a widget that does not implement it. */
static void case_mixer(void)
{
    struct graph g = {0};
    add_widget(&g, 20, WIDGET_ADC);
    add_widget(&g, 21, WIDGET_MIXER);
    add_pin(&g, 22, (1u << 5), 0x0);
    add_pin(&g, 23, (1u << 4), 0x0);   /* an unrelated OUTPUT-only pin on the mixer */
    wire(&g, 21, 22);
    wire(&g, 21, 23);
    wire(&g, 20, 21);

    unsigned adcs[8], nadc, inpins[8], ninpin;
    gather(&g, adcs, &nadc, inpins, &ninpin);
    CHECK(ninpin == 1 && inpins[0] == 22, "mixer: only the input-capable pin gathered");

    struct result r;
    int ok = try_capture_path(&g, adcs, nadc, inpins, ninpin, &r);
    CHECK(ok == 1, "mixer: capture path found");
    CHECK(r.cap_pin_nid == 22, "mixer: resolves to pin 22");
    CHECK(r.cap_sel_nid == 0, "mixer: no select programmed (a mixer sums, it does not choose)");
}

/* Case 4: the pin exists, is input-capable, but its port connectivity says
 * "no physical connection" (0x1) -- the exact bit find_output_path already
 * checks for the OUTPUT side. It must never even become a CANDIDATE, not
 * merely fail to resolve -- ninpin must be 0, so a caller cannot mistake
 * "found nothing to search" for "searched and refused". */
static void case_disconnected_pin(void)
{
    struct graph g = {0};
    add_widget(&g, 30, WIDGET_ADC);
    add_pin(&g, 31, (1u << 5), 0x1);   /* input-capable, but NOT physically connected */
    wire(&g, 30, 31);

    unsigned adcs[8], nadc, inpins[8], ninpin;
    gather(&g, adcs, &nadc, inpins, &ninpin);
    CHECK(ninpin == 0, "disconnected: the unconnected pin is never gathered as a candidate");

    struct result r;
    int ok = try_capture_path(&g, adcs, nadc, inpins, ninpin, &r);
    CHECK(ok == 0, "disconnected: capture path refused");
    CHECK(r.adc_nid == 0, "disconnected: adc_nid left at 0 (not silently wired anyway)");
}

/* Case 5: an ADC and an input pin both exist, and are NOT connected to each
 * other -- the ADC's connection list points somewhere unrelated (a DAC, in
 * this table, standing in for "anything that is not the pin"). This is the
 * case the file header calls out explicitly: unlike the output side, there
 * is NO "wire the first ADC to the first pin" fallback, because that guess
 * would read a source that is not actually the microphone. */
static void case_no_edge_refuses(void)
{
    struct graph g = {0};
    add_widget(&g, 40, WIDGET_ADC);
    add_widget(&g, 41, WIDGET_DAC);      /* something else entirely */
    add_pin(&g, 42, (1u << 5), 0x0);     /* a real, connected input pin -- just not wired to 40 */
    wire(&g, 40, 41);                     /* ADC listens to the DAC?! -- nonsense, but present */

    unsigned adcs[8], nadc, inpins[8], ninpin;
    gather(&g, adcs, &nadc, inpins, &ninpin);
    CHECK(nadc == 1 && ninpin == 1, "no-edge: one ADC and one candidate pin exist");

    struct result r;
    int ok = try_capture_path(&g, adcs, nadc, inpins, ninpin, &r);
    CHECK(ok == 0, "no-edge: refused rather than guessing ADC 40 <- pin 42");
    CHECK(r.adc_nid == 0, "no-edge: adc_nid left unset -- capture stays OFF, not silently misrouted");
}

/* Case 6: two ADCs; only the SECOND one's connection list actually reaches
 * the input pin. Pins the search to picking the provably-connected ADC over
 * the first one merely because it came first in nid order. */
static void case_second_adc_wins(void)
{
    struct graph g = {0};
    add_widget(&g, 50, WIDGET_ADC);
    add_widget(&g, 51, WIDGET_DAC);
    add_widget(&g, 52, WIDGET_ADC);
    add_pin(&g, 53, (1u << 5), 0x0);
    wire(&g, 50, 51);      /* ADC 50 listens to something irrelevant */
    wire(&g, 52, 53);      /* ADC 52 actually reaches the input pin */

    unsigned adcs[8], nadc, inpins[8], ninpin;
    gather(&g, adcs, &nadc, inpins, &ninpin);
    CHECK(nadc == 2, "second-adc: both ADCs gathered");

    struct result r;
    int ok = try_capture_path(&g, adcs, nadc, inpins, ninpin, &r);
    CHECK(ok == 1, "second-adc: capture path found");
    CHECK(r.adc_nid == 52, "second-adc: the CONNECTED adc (52) is chosen, not the first-seen (50)");
}

int main(void)
{
    case_direct();
    case_selector();
    case_mixer();
    case_disconnected_pin();
    case_no_edge_refuses();
    case_second_adc_wins();

    if (g_fail) {
        printf("\n%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("\nHDA_CAPTURE_GRAPH_OK\n");
    return 0;
}
