/* Private startup snapshots for the platform and HTML-semantics installers.
 * Guest attribution (style-js-before/serial.txt, 2026-09-09): Wikipedia spent
 * 940/560 ms in platform/semantics versus 60/40 ms on the lean document. Their
 * startup iframe/details searches crossed into JS for EVERY unrelated node.
 * Walk native links once and create wrappers only for matches; the original
 * initializer still decides frame policy and details state. This is neither a
 * public selector API nor a replacement for live DOM traversal after startup.
 * Host real-install gate: 2400 unrelated elements caused 12144 DOM_ST_WRAP
 * events with the old scans and 114 with native filtering; details and parser
 * iframe assertions passed in both (make test-bootstrap-scan, control first).
 *
 * Traverse the ordinary child tree, not the composed tree: the replaced JS
 * firstChild/nextSibling searches did not enter shadow roots either. An
 * iterative walk avoids adding C recursion limits on deeply nested input. */
static JSValue js_bootstrap_snapshot(JSContext *ctx, const char *tag, int fold)
{
#ifdef JS_BOOTSTRAP_JS_SCAN
    /* Negative control: let each installer use its original JS full-tree scan.
     * Functionality stays intact; DOM_ST_WRAP must expose the extra work. */
    (void)ctx; (void)tag; (void)fold;
    return JS_UNDEFINED;
#else
    JSValue out=JS_NewArray(ctx);
    if(JS_IsException(out))return out;
    struct node *root=js_dom_root(), *n=root;
    uint32_t index=0;
    while(n) {
        int matches=n->type==N_ELEM&&n->tag;
        if(matches) {
            int i=0;
            for(;;i++) {
                int a=(unsigned char)n->tag[i], b=(unsigned char)tag[i];
                if(fold&&a>='A'&&a<='Z')a+='a'-'A';
                if(a!=b){matches=0;break;}
                if(!a)break;
            }
        }
        if(matches) {
            JSValue node=js_dom_node_value(ctx,n);
            if(JS_IsException(node)){JS_FreeValue(ctx,out);return node;}
            if(JS_SetPropertyUint32(ctx,out,index++,node)<0){JS_FreeValue(ctx,out);return JS_EXCEPTION;}
        }
        if(n->first_child){n=n->first_child;continue;}
        while(n!=root&&!n->next)n=n->parent;
        if(n==root)break;
        n=n->next;
    }
    return out;
#endif
}
