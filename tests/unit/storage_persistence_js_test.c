#define main webapi_existing_main
#include "webapi_test.c"
#undef main
#include "storage_backend.h"
#include "tabs.h"
static unsigned char saved[2][8192];static int saved_len[2],write_error;
static int read_store(const char *path,void *buf,int max){int slot=path[strlen(path)-1]=='1';if(!saved_len[slot])return -1;int n=saved_len[slot];if(n>max)return -1;memcpy(buf,saved[slot],n);return n;}
static int write_store(const char *path,const void *buf,int n){int slot=path[strlen(path)-1]=='1';if(write_error || n>(int)sizeof saved[slot])return -1;memcpy(saved[slot],buf,n);saved_len[slot]=n;return n;}
static int mkdir_store(const char *p){(void)p;return 0;}
static const struct bstore_ops ops={read_store,write_store,mkdir_store};
int main(void)
{
 ck(js_webapi_set_storage_store(&ops)==0,"binding attaches persistent store");open_ctx("https://persist.example/");
 run("localStorage.setItem('key','saved');sessionStorage.setItem('tab','private');");
 ckjs("localStorage.getItem('key')==='saved'","binding reads committed bytes");
 close_ctx();ck(js_webapi_set_storage_store(&ops)==0,"binding reopens store");open_ctx("https://persist.example/");
 ckjs("localStorage.getItem('key')==='saved' && sessionStorage.getItem('tab')===null","binding reopen restores local but not session");
 write_error=1;run("var error='';try{localStorage.setItem('key','lost')}catch(e){error=e.name}");
 ckjs("error==='InvalidStateError' && localStorage.getItem('key')==='saved'","failed persistent set throws and preserves old map");
 run("error='';try{localStorage.removeItem('key')}catch(e){error=e.name}");
 ckjs("error==='InvalidStateError' && localStorage.getItem('key')==='saved'","failed persistent remove throws and preserves old map");
 run("error='';try{localStorage.clear()}catch(e){error=e.name}");
 ckjs("error==='InvalidStateError' && localStorage.getItem('key')==='saved'","failed persistent clear throws and preserves old map");
 run("sessionStorage.setItem('tab','still live')");ckjs("sessionStorage.getItem('tab')==='still live'","disk failure leaves session storage usable");
 close_ctx();write_error=0;ck(js_webapi_set_storage_store(&ops)==0,"reopen releases failed commit latch");open_ctx("https://persist.example/");
 run("localStorage.removeItem('key')");close_ctx();ck(js_webapi_set_storage_store(&ops)==0,"removed store reopens");open_ctx("https://persist.example/");ckjs("localStorage.getItem('key')===null","removed key remains absent after reopen");close_ctx();
 for(int i=0;i<2;i++)if(saved_len[i])saved[i][0]^=1;
 ck(js_webapi_set_storage_store(&ops)==STORAGE_CORRUPT,"binding reports corrupt snapshot state");open_ctx("https://persist.example/");
 ckjs("(function(){try{localStorage.getItem('key')}catch(e){return e.name==='InvalidStateError'}return false})()","corrupt local get does not masquerade as empty state");
 ckjs("(function(){try{return localStorage.length<0}catch(e){return e.name==='InvalidStateError'}})()","corrupt local length does not masquerade as empty state");
 ckjs("(function(){try{localStorage.key(0)}catch(e){return e.name==='InvalidStateError'}return false})()","corrupt local key reports failure");
 run("sessionStorage.setItem('tab','works')");ckjs("sessionStorage.getItem('tab')==='works'","corrupt local snapshots do not block session");
 close_ctx();js_webapi_set_storage_store(NULL);printf("storage-persistence-js: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
