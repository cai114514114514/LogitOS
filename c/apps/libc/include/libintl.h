#ifndef _LIBINTL_H
#define _LIBINTL_H

/* gettext's no-catalog path is still a useful contract: ports may keep their
 * normal localization calls and receive the original msgid.  LogitOS does not
 * yet install MO catalogs, so these functions never claim a translation was
 * loaded; see intl.c for the queryable domain/directory bookkeeping. */

char *gettext(const char *msgid);
char *dgettext(const char *domainname, const char *msgid);
char *dcgettext(const char *domainname, const char *msgid, int category);
char *ngettext(const char *singular, const char *plural, unsigned long n);
char *dngettext(const char *domainname, const char *singular,
                const char *plural, unsigned long n);
char *dcngettext(const char *domainname, const char *singular,
                 const char *plural, unsigned long n, int category);
char *textdomain(const char *domainname);
char *bindtextdomain(const char *domainname, const char *dirname);
char *bind_textdomain_codeset(const char *domainname, const char *codeset);

#endif /* _LIBINTL_H */
