/* Native frame opening uses the URL Standard parser already shipped with the
 * browser. Small loader-only host links omit it; keep that absence explicit,
 * instead of falling back to url.c which silently truncates path/authority.
 * The real action gate links the full production parser. */
#include "js_url.h"
extern urlrec *url_parse_w(const char *,int,const urlrec *) LOGIT_WEAK;
extern char *url_get(const urlrec *,int) LOGIT_WEAK;
extern void url_free_w(urlrec *) LOGIT_WEAK;
LOGIT_WEAK_STUB(url_parse_w);
LOGIT_WEAK_STUB(url_get);
LOGIT_WEAK_STUB(url_free_w);

static int open_embedded_page(struct node *n)
{
#ifdef BROWSER_FRAME_OPEN_NO_ACTION
    (void)n; return 0;
#else
    const char *src=frame_open_source(n), *committed=committed_page_url();
    if (!src || !committed || strlen(committed)>=TAB_URL ||
        !LOGIT_HAVE(url_parse_w) || !LOGIT_HAVE(url_get) || !LOGIT_HAVE(url_free_w)) return 0;
    /* Copy both borrowed values before any navigation retires their document.
     * Same-tab load_from preserves the top-level sessionStorage partition and
     * classifies this as a page-initiated navigation, not typed browser chrome.
     * This does not keep the old page's JS realm or promise an OAuth callback. */
    char source[TAB_URL], initiator[TAB_URL], target[TAB_URL];
    strcpy(source,src); strcpy(initiator,committed);
    urlrec *base=url_parse_w(initiator,-1,0);
    urlrec *dest=url_parse_w(source,-1,base);
    if(base)url_free_w(base);
    if(!dest)return 0;
    char *href=url_get(dest,URLC_HREF), *protocol=url_get(dest,URLC_PROTOCOL);
    char *host=url_get(dest,URLC_HOSTNAME), *path=url_get(dest,URLC_PATHNAME);
    char *query=url_get(dest,URLC_SEARCH), *hash=url_get(dest,URLC_HASH);
    char *user=url_get(dest,URLC_USERNAME), *pass=url_get(dest,URLC_PASSWORD);
    /* The transport has smaller component buffers than the URL Standard
     * parser. Reject before load_from: its legacy parser otherwise accepts a
     * truncated OAuth state or a different authority as a successful URL. */
    int ok=href && protocol && host && path && query && hash && user && pass &&
        (!strcmp(protocol,"http:") || !strcmp(protocol,"https:")) &&
        !user[0] && !pass[0] && host[0] && strlen(host)<URL_HOST_MAX &&
        !strchr(host,':') && strlen(path)+strlen(query)+strlen(hash)<URL_PATH_MAX &&
        strlen(href)<sizeof target;
    if(ok)strcpy(target,href);
    free(href);free(protocol);free(host);free(path);free(query);free(hash);free(user);free(pass);
    url_free_w(dest);
    if(!ok)return 0;
    strcpy(url,target);ulen=(int)strlen(url);addr_sync();hist_push(url);
    load_from(target,initiator);
    return 1;
#endif
}
