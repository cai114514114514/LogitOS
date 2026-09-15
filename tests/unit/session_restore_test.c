/* The real tabs store round-trip and one-shot restore intent. This does not
 * claim a browser repaint: browser.c must consume after final layout, while
 * preserving pending positions if the tab is left before its first load. */
#include <stdio.h>
#include <string.h>
#include "tabs.h"
static char disk[16384];
static int disklen, checks, fails;
static int rd(const char *p,void *b,int cap) { if(strcmp(p,SESSION_PATH))return -1; int n=disklen<cap?disklen:cap;memcpy(b,disk,n);return n; }
static int wr(const char *p,const void *b,int n) { if(strcmp(p,SESSION_PATH)||n>=(int)sizeof disk)return -1;memcpy(disk,b,n);disklen=n;disk[n]=0;return 0; }
static int md(const char *p) { (void)p;return 0; }
static const struct bstore_ops store={rd,wr,md};
#define CHECK(c,m) do { checks++;if(!(c)){fails++;printf("FAIL: %s\n",m);} }while(0)
static void restore(const char *s) { tabs_init();disklen=(int)strlen(s);memcpy(disk,s,disklen+1);CHECK(session_restore()==1,"one disk tab restored"); }
int main(void) {
    tabs_set_store(&store);tabs_init();int i=tabs_new("https://example.test/a");struct tab *t=tab_at(i);
    CHECK(!t->restore_pending,"new tab has no restore intent");t->scroll=240;t->scroll_x=360;
    CHECK(session_save()==0,"write new format");
    CHECK(strstr(disk,"\t240\t360\n")!=0,"new format persists both axes");
    tabs_init();CHECK(session_restore()==1,"round-trip restores tab");t=tab_cur();
    CHECK(!t->src && !t->loaded,"disk tab has no cached source or live document");
    CHECK(t->scroll==240 && t->scroll_x==360,"round-trip restores both coordinates");
    CHECK(t->restore_pending,"disk position awaits first layout");
    tab_restore_begin(t,"https://example.test/a");tab_drop_content(t);
    CHECK(t->restore_pending,"same URL and failed-load byte cleanup retain intent");
    int x=-1,y=-1;CHECK(tab_restore_take(t,&x,&y),"first successful layout consumes restore");
    CHECK(x==360 && y==240,"consume yields original disk position");
    CHECK(!t->restore_pending,"consumed restore is cleared");
    x=y=-1;CHECK(!tab_restore_take(t,&x,&y),"restore cannot run twice");
    CHECK(x==-1 && y==-1,"absent restore leaves caller coordinates alone");
    restore("logit-browser-session\t1\t0\n0\thttps://example.test/old\tOld\t180\n");t=tab_cur();
    CHECK(t->scroll_x==0 && t->scroll==180,"old four-column format defaults x to zero");
    CHECK(tab_restore_take(t,&x,&y) && x==0 && y==180,"old format also restores on first layout");
    restore("logit-browser-session\t1\t0\n0\thttps://example.test/a\tNew\t240\t360\n");t=tab_cur();
    tab_restore_begin(t,"https://example.test/b");
    CHECK(!t->restore_pending && t->scroll_x==0 && t->scroll==0,"different user navigation cancels saved position");
    CHECK(!tab_restore_take(t,&x,&y),"different document cannot consume old position");
    restore("logit-browser-session\t1\t0\n0\thttps://example.test/a\tNew\t240\t360\n");t=tab_cur();
    CHECK(session_save()==0,"save unopened restored tab");tabs_init();CHECK(session_restore()==1,"restore saved unopened tab");
    CHECK(tab_cur()->restore_pending && tab_cur()->scroll_x==360,"unopened restore survives another session");
    tabs_close(tabs_active());CHECK(!tab_cur()->restore_pending,"closing restored tab clears intent on reused slot");
    tabs_init();CHECK(tabs_count()==0,"tabs_init clears restored state");
    printf("session-restore: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
