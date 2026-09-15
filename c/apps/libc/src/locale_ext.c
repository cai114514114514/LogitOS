/* POSIX locale objects over LogitOS's one real locale.  The object API is not
 * faked for names we cannot load: newlocale still rejects every non-C name.
 * A C-locale handle is useful because portable parsers can now use *_l calls
 * without mutating process-global state. */
#include <locale.h>
#include <langinfo.h>
#include <errno.h>
#include <string.h>

struct __locale_struct { unsigned int magic; };
static struct __locale_struct c_locale = { 0x4c4f434cU };
static _Thread_local locale_t thread_locale;

static int locale_name_ok(const char *name)
{ return name && (!*name || !strcmp(name, "C") || !strcmp(name, "POSIX")); }

locale_t newlocale(int mask, const char *name, locale_t base)
{
    if (mask & ~LC_ALL_MASK || base == LC_GLOBAL_LOCALE || !locale_name_ok(name)) {
        errno = EINVAL;
        return (locale_t)0;
    }
    (void)base;
    return &c_locale;
}

locale_t duplocale(locale_t loc)
{
    if (!loc) { errno = EINVAL; return (locale_t)0; }
    return &c_locale;
}

void freelocale(locale_t loc) { (void)loc; }

locale_t uselocale(locale_t loc)
{
    locale_t old = thread_locale ? thread_locale : LC_GLOBAL_LOCALE;
    if (!loc) return old;
    if (loc == LC_GLOBAL_LOCALE) thread_locale = (locale_t)0;
    else thread_locale = &c_locale;
    return old;
}

struct lconv *localeconv_l(locale_t loc) { (void)loc; return localeconv(); }
char *nl_langinfo_l(nl_item item, locale_t loc) { (void)loc; return nl_langinfo(item); }
