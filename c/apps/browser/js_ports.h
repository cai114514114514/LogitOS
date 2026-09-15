#ifndef LOGIT_JS_PORTS_H
#define LOGIT_JS_PORTS_H
#include "quickjs.h"
#include "../../../include/weaksym.h"
#ifdef JS_PORTS_OPTIONAL
#define PORT_FN LOGIT_WEAK
#else
#define PORT_FN
#endif
/* Native, bounded endpoint ownership across independent runtimes. Pump only
 * the current realm under its watchdog. In-transit packets contain no JSValue.
 * The first subset transfers ports through MessageEvent.ports; native ports
 * nested in data and ArrayBuffer transfer remain explicit clone failures. */
struct js_port_packet;
PORT_FN int js_ports_install(JSContext *);
PORT_FN void js_ports_close(JSContext *);
PORT_FN int js_ports_pending(JSContext *);
PORT_FN int js_ports_pump(JSContext *);
PORT_FN struct js_port_packet *js_ports_prepare(JSContext *,JSValueConst data,JSValueConst transfer);
PORT_FN int js_ports_commit(JSContext *,struct js_port_packet *);
PORT_FN JSValue js_ports_read(JSContext *,struct js_port_packet *,JSValue *ports);
PORT_FN void js_ports_discard(struct js_port_packet *);
PORT_FN size_t js_ports_packet_size(const struct js_port_packet *);
#ifdef JS_PORTS_OPTIONAL
LOGIT_WEAK_STUB(js_ports_install);
LOGIT_WEAK_STUB(js_ports_close);
LOGIT_WEAK_STUB(js_ports_pending);
LOGIT_WEAK_STUB(js_ports_pump);
LOGIT_WEAK_STUB(js_ports_prepare);
LOGIT_WEAK_STUB(js_ports_commit);
LOGIT_WEAK_STUB(js_ports_read);
LOGIT_WEAK_STUB(js_ports_discard);
LOGIT_WEAK_STUB(js_ports_packet_size);
#endif
#undef PORT_FN
#endif
