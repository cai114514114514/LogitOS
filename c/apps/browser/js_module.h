#ifndef LOGIT_JS_MODULE_H
#define LOGIT_JS_MODULE_H

/* ES modules for the page runtime.
 *
 * THE MEASUREMENT THAT PUT THIS FILE HERE. kimi.com ships exactly one script:
 *
 *   <script type="module" crossorigin
 *           src="//statics.moonshot.cn/kimi-web-seo/assets/index-h6DE6Ow7.js">
 *
 * 1,623,655 bytes. Compiled with the same QuickJS this project links:
 *
 *   JS_EVAL_TYPE_GLOBAL   (what browser.c did):  SyntaxError at byte ~0
 *   JS_EVAL_TYPE_MODULE:                         all 1.55 MB parsed in 45 ms
 *                                                (34 MB/s), then stopped at LINK
 *                                                time wanting a sibling chunk
 *
 * So the engine was never the wall. The browser evaluated everything as a
 * classic script, and a classic script that starts with `import` is a syntax
 * error on its first token. Every Vite/Rollup/Next build on the web ships as a
 * module graph, so this one gap took all of them out at once -- and it looked
 * like "QuickJS cannot handle real bundles", which is the opposite of true.
 *
 * What is here is the two halves that were missing: evaluate a `<script
 * type="module">` with JS_EVAL_TYPE_MODULE, and give the runtime a module
 * loader so `import "./chunk.js"` resolves against the IMPORTING module's URL
 * and is fetched. */

struct node;

/* Evaluate one module. `url` is the module's own URL -- it becomes the module
 * name (so a module imported twice is instantiated once) and the base every
 * specifier inside it resolves against, so it must be unique and absolute. For
 * an inline <script type="module">, pass the page URL with a "#inline-N"
 * discriminator. Returns 1 if it evaluated without an uncaught exception.
 *
 * Installs the runtime's module loader on first use, and re-installs it after
 * a navigation replaced the runtime. */
int js_module_eval(const char *src, int len, const char *url);

/* Modules are deferred: they run after the document is parsed, in document
 * order, after every classic script. The browser calls this once the classic
 * pass is over. Returns how many modules ran. */

/* True if `type` (the raw value of a <script type=...> attribute, possibly
 * NULL) selects the module goal. */
int js_module_is_module_type(const char *type);
/* True if `type` names a classic script (NULL, empty, or a JavaScript MIME
 * type). Anything else -- importmap, application/json, text/template -- is a
 * data block the spec says must NOT be executed. */
int js_module_is_classic_type(const char *type);

/* How many modules were fetched by the loader for the current page, and how
 * many of those failed. Reset per navigation by js_module_reset(). */
void js_module_stats(int *loaded, int *failed);
void js_module_reset(void);

#endif /* LOGIT_JS_MODULE_H */
