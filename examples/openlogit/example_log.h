#ifndef OPENLOGIT_EXAMPLE_LOG_H
#define OPENLOGIT_EXAMPLE_LOG_H
#include "logit.h"
#include <stdarg.h>
#include <stdio.h>
/* A guest serial line must be one write. printf can emit each format fragment
 * separately; a kernel diagnostic between `pause=` and `0` then destroys the
 * evidence even though the input was handled. Truncation remains explicit. */
static void example_log(const char *format,...)
{
    char line[512];va_list ap;va_start(ap,format);
    int n=vsnprintf(line,sizeof line,format,ap);va_end(ap);
    if(n<0)return;
    if(n>=(int)sizeof line){n=sizeof line-1;line[n-1]='\n';}
    _sys(SYS_WRITE,1,(long)line,n);
}
#endif
