/* gettext without installed catalogs.  Returning msgid is the specified
 * fallback, not a fabricated translation.  Domain and binding setters retain
 * queryable state because build systems commonly use them to detect that the
 * API is alive even when no catalog is shipped. */
#include <libintl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define INTL_BINDINGS 16
struct binding { char *domain, *dir, *codeset; };
static struct binding bindings[INTL_BINDINGS];
static char *current_domain;

static char *copy_string(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static struct binding *binding_for(const char *domain, int create)
{
    struct binding *empty = NULL;
    for (int i = 0; i < INTL_BINDINGS; i++) {
        if (bindings[i].domain && !strcmp(bindings[i].domain, domain)) return &bindings[i];
        if (!bindings[i].domain && !empty) empty = &bindings[i];
    }
    if (!create || !empty) return NULL;
    empty->domain = copy_string(domain);
    return empty->domain ? empty : NULL;
}

char *gettext(const char *msgid) { return (char *)msgid; }
char *dgettext(const char *domainname, const char *msgid)
{ (void)domainname; return (char *)msgid; }
char *dcgettext(const char *domainname, const char *msgid, int category)
{ (void)domainname; (void)category; return (char *)msgid; }
char *ngettext(const char *singular, const char *plural, unsigned long n)
{ return (char *)(n == 1 ? singular : plural); }
char *dngettext(const char *domainname, const char *singular,
                const char *plural, unsigned long n)
{ (void)domainname; return ngettext(singular, plural, n); }
char *dcngettext(const char *domainname, const char *singular,
                 const char *plural, unsigned long n, int category)
{ (void)domainname; (void)category; return ngettext(singular, plural, n); }

char *textdomain(const char *domainname)
{
    static char default_domain[] = "messages";
    if (!domainname) return current_domain ? current_domain : default_domain;
    if (!*domainname) { free(current_domain); current_domain = NULL; return default_domain; }
    char *next = copy_string(domainname);
    if (!next) { errno = ENOMEM; return NULL; }
    free(current_domain);
    current_domain = next;
    return current_domain;
}

char *bindtextdomain(const char *domainname, const char *dirname)
{
    static char default_dir[] = "/usr/share/locale";
    if (!domainname || !*domainname) { errno = EINVAL; return NULL; }
    struct binding *b = binding_for(domainname, dirname != NULL);
    if (!dirname) return b && b->dir ? b->dir : default_dir;
    if (!b) { errno = ENOMEM; return NULL; }
    char *next = copy_string(dirname);
    if (!next) { errno = ENOMEM; return NULL; }
    free(b->dir); b->dir = next;
    return b->dir;
}

char *bind_textdomain_codeset(const char *domainname, const char *codeset)
{
    if (!domainname || !*domainname) { errno = EINVAL; return NULL; }
    struct binding *b = binding_for(domainname, codeset != NULL);
    if (!codeset) return b ? b->codeset : NULL;
    if (!b) { errno = ENOMEM; return NULL; }
    char *next = copy_string(codeset);
    if (!next) { errno = ENOMEM; return NULL; }
    free(b->codeset); b->codeset = next;
    return b->codeset;
}
