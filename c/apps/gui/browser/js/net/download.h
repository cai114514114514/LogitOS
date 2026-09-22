/* SPDX-License-Identifier: MIT */
#ifndef LOGIT_JS_DOWNLOAD_H
#define LOGIT_JS_DOWNLOAD_H
#include "quickjs.h"
typedef int (*js_download_handler)(const char *,const char *,const unsigned char *,int);
void js_download_set_handler(js_download_handler handler);
void js_download_install(JSContext *ctx);
#endif
