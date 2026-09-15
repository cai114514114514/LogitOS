#define main loader_original_main
#include "loader_test.c"
#undef main
#include "js_dom.h"
void progress_paint_tick(void);
int browser_settle(void);
void loader_poll_hook(void) { }

int main(void)
{
    fake_site_reset();
    fake_site_add("http://fixture.test/progress", "<!doctype html><style>"
        "#old:before{content:'Prefix';display:block}</style>"
        "<body><div id=old>CommittedFrame</div></body>");
    browser_load("http://fixture.test/progress");
    CHECK(painted_text("CommittedFrame"), "progress apparatus initially painted real page text");
    js_dom_clear_dirty(); host_clock+=4000; paint_nops=0;
    progress_paint_tick();
    CHECK(paint_nops>0, "clean progress callback can repaint committed page");
    const char *src="var added=document.createElement('p');added.textContent='NewCommittedText';document.body.appendChild(added);";
    CHECK(js_page_eval(src,(int)strlen(src),"<progress fixture>",0), "ordinary script appends a node");
    CHECK(js_dom_dirty(), "script left an unsettled DOM invalidation");
    /* Do not free the old display-list sources in this negative control. A
     * stale paint is measurable without manufacturing a dangling-pointer
     * dereference: even this safe append must wait for the next commit. */
    host_clock+=4000; paint_nops=0;
    progress_paint_tick();
    CHECK(paint_nops==0, "progress callback does not paint an unsettled DOM");
    CHECK(js_dom_dirty(), "progress callback does not re-enter layout or consume the mutation");
    CHECK(browser_settle(), "outer browser settle commits the new frame");
    host_clock+=4000; paint_nops=0;progress_paint_tick();
    CHECK(painted_text("NewCommittedText"), "committed replacement is painted after settling");
    js_page_close();
    puts(fail?"progress-paint: FAIL":"progress-paint: PASS");return fail?1:0;
}
