#ifndef LOGIT_JS_CHARACTERDATA_H
#define LOGIT_JS_CHARACTERDATA_H

#include "quickjs.h"

/* Install CharacterData's remaining mutation methods -- insertData,
 * deleteData, replaceData -- and Text.splitText, over the real
 * CharacterData.prototype / Text.prototype js_dom_iface.inc already
 * publishes. appendData, substringData, `data` and `length` are NOT
 * redefined here: they already exist (js_dom_iface.inc's cd_appendData /
 * cd_substringData, js_dom.c's el_get_nodeValue / el_set_nodeValue), this
 * file only adds what was genuinely absent.
 *
 * Called from js_select_install() rather than from js_page.c, on the same
 * reasoning js_tokenlist_install is: js_page.c is edited by several lines at
 * once and this needs no ordering of its own beyond "after js_dom_init", which
 * js_select already has.
 *
 * Idempotent: a second call is a no-op. */
void js_characterdata_install(JSContext *ctx);

#endif /* LOGIT_JS_CHARACTERDATA_H */
