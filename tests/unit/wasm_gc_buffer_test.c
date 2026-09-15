/* SPDX-License-Identifier: MIT
 * Defense-only control: never read potentially freed bytes. Native slot
 * counts prove collection occurred; byteLength proves whether the retained
 * wrapper was safely detached. Full buffer-owned backing storage is separate.
 */
#define main wasm_realms_main
#include "wasm_realms_test.c"
#undef main

int main(void)
{
    /* (module (memory (export "mem") 1)): no exported function closures,
     * so this isolates the memory lifetime from function/WeakMap cycles. */
    static const unsigned char memory_only[]={0,97,115,109,1,0,0,0,
        5,3,1,0,1,7,7,1,3,109,101,109,2,0};
    int failed=0;
    for(int k=0;k<2;k++){
        struct realm r=open_realm(NULL);
        put_bytes(r.ctx,"memoryOnly",memory_only,sizeof memory_only);
        const char *src=k?
            "globalThis.keep=new WebAssembly.Instance(new WebAssembly.Module(memoryOnly));"
            "globalThis.retained=keep.exports.mem.buffer;globalThis.view=new Uint8Array(retained);true":
            "globalThis.keep=new WebAssembly.Memory({initial:1});"
            "globalThis.retained=keep.buffer;globalThis.view=new Uint8Array(retained);true";
        if(!check(r.ctx,src,"retained view setup"))return 2;
        if(js_wasm_test_live_slots(r.ctx,3)!=1 || (k&&js_wasm_test_live_slots(r.ctx,2)!=1))return 2;
        if(!check(r.ctx,"retained.byteLength===65536&&view.length===65536","initial buffer length"))return 2;
        if(!check(r.ctx,"keep=null;true","release Wasm wrapper"))return 2;
        JS_RunGC(r.rt);JS_RunGC(r.rt);
        /* Inspect metadata only. Do not run any view read/write/copy even if
         * the negative build leaves byteLength looking live. */
        if(js_wasm_test_live_slots(r.ctx,3)!=0 || (k&&js_wasm_test_live_slots(r.ctx,2)!=0)){
            printf("FAIL GC: %s collection not observed\n",k?"instance":"memory");failed++;
        }else if(!check(r.ctx,"retained.byteLength===0&&view.length===0","collected storage detaches retained wrappers")){
            printf("FAIL GC: %s stale wrapper length\n",k?"instance":"memory");failed++;
        }else printf("PASS GC: %s safely retires retained wrapper\n",k?"instance":"memory");
        close_realm(&r);
        if(js_wasm_test_owner_count()!=0)return 2;
    }
    printf("wasm-gc-buffer: 2 cases, %d failures\n",failed);
    return failed?1:0;
}
