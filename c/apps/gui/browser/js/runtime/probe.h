#ifndef JS_RUNTIME_PROBE_H
#define JS_RUNTIME_PROBE_H
/* Diagnostic builds only (-DJS_PROBE_TRACE): see probe.c. */
#ifdef JS_PROBE_TRACE
#include "quickjs.h"
void js_probe_install(JSContext *ctx, const char *prefix);   /* after the platform is installed */
void js_probe_report(void);                                  /* every event, with counts */
void js_probe_reset(JSContext *ctx);                         /* before ctx is freed */
#endif
#endif
