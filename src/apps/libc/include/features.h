#ifndef _FEATURES_H
#define _FEATURES_H
/* musl internal attribute macros (libm.h / *_data.h rely on these). */
#define weak       __attribute__((__weak__))
#define hidden     __attribute__((__visibility__("hidden")))
#define weak_alias(old, new) \
    extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))
#endif
