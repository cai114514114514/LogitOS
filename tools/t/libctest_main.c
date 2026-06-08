/* /bin/libctest -- on-target mini-libc test battery. Links the real mini-libc
 * (src/apps/libc) and exercises it under Aether; prints "LIBC_OK <n>/<n>" on
 * success or "LIBC_FAIL" with details. Run by `make test-libc` over the serial
 * shell (scripts/run-libc-test.sh). Host name-clashes (string.c defines memcpy
 * etc.) make a native test awkward, so this runs in the real environment. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

static int checks, fails;
#define CHK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s\n", msg); } } while (0)
#define CHK_STR(got, want, msg) do { checks++; if (strcmp((got),(want)) != 0) { fails++; printf("FAIL: %s -- got '%s' want '%s'\n", msg, (got), (want)); } } while (0)
#define CHK_INT(got, want, msg) do { checks++; long g=(long)(got), w=(long)(want); if (g != w) { fails++; printf("FAIL: %s -- got %ld want %ld\n", msg, g, w); } } while (0)

static void t_string(void)
{
    char b[64];
    CHK_INT(strlen("hello"), 5, "strlen");
    CHK_INT(strnlen("hello", 3), 3, "strnlen cap");
    CHK(strcmp("abc","abc") == 0, "strcmp eq");
    CHK(strcmp("abc","abd") < 0, "strcmp lt");
    CHK_INT(strncmp("abcX","abcY",3), 0, "strncmp eq3");
    strcpy(b, "copy"); CHK_STR(b, "copy", "strcpy");
    memset(b, 'x', 5); b[5]=0; CHK_STR(b, "xxxxx", "memset");
    /* strncpy zero-pads to n when src is shorter */
    memset(b, '?', sizeof b); strncpy(b, "ab", 6); CHK(b[0]=='a'&&b[1]=='b'&&b[2]==0&&b[3]==0&&b[5]==0, "strncpy pad");
    strcpy(b, "foo"); strcat(b, "bar"); CHK_STR(b, "foobar", "strcat");
    strcpy(b, "foo"); strncat(b, "barbaz", 3); CHK_STR(b, "foobar", "strncat n");
    CHK(strchr("hello",'l') == strstr("hello","llo"), "strchr/strstr point same");
    CHK(strrchr("hello",'l')[1]=='o', "strrchr last l");
    CHK(strstr("hello","xy") == 0, "strstr miss");
    CHK_INT(memcmp("abc","abd",3), -1<0?-1:1, "memcmp sign");  /* sign only */
    CHK(memcmp("abc","abc",3)==0, "memcmp eq");
    /* memmove overlap (forward + backward) */
    strcpy(b, "abcdef"); memmove(b+2, b, 4); b[6]=0; CHK_STR(b, "ababcd", "memmove overlap fwd");
    strcpy(b, "abcdef"); memmove(b, b+2, 4); CHK(b[0]=='c'&&b[3]=='f', "memmove overlap bwd");
    CHK_INT(memcmp(memcpy(b,"XYZ",3),"XYZ",3), 0, "memcpy");
    CHK(memchr("hello",'l',5) != 0, "memchr hit");
    CHK(memchr("hello",'z',5) == 0, "memchr miss");
    CHK_INT(strspn("aabbc","ab"), 4, "strspn");
    CHK_INT(strcspn("abcde","cd"), 2, "strcspn");
    CHK(strpbrk("hello","xl") != 0 && *strpbrk("hello","xl")=='l', "strpbrk");
    char tk[] = "a,b,,c"; char *sp=0; char *p = strtok_r(tk, ",", &sp);
    CHK_STR(p, "a", "strtok_r 1"); p=strtok_r(0,",",&sp); CHK_STR(p,"b","strtok_r 2");
    char *d = strdup("dup"); CHK(d && strcmp(d,"dup")==0, "strdup"); free(d);
    CHK(isspace(' ') && !isspace('x'), "isspace"); /* sanity for ctype too */
}

static void t_ctype(void)
{
    CHK(isdigit('7') && !isdigit('a'), "isdigit");
    CHK(isalpha('Z') && !isalpha('5'), "isalpha");
    CHK(isalnum('a') && isalnum('0') && !isalnum('-'), "isalnum");
    CHK(isxdigit('f') && isxdigit('9') && !isxdigit('g'), "isxdigit");
    CHK(isupper('A') && !isupper('a'), "isupper");
    CHK(islower('z') && !islower('Z'), "islower");
    CHK(isspace('\t') && isspace('\n') && !isspace('q'), "isspace2");
    CHK_INT(toupper('a'), 'A', "toupper"); CHK_INT(toupper('A'), 'A', "toupper idem");
    CHK_INT(tolower('Z'), 'z', "tolower"); CHK_INT(tolower('z'), 'z', "tolower idem");
    CHK(ispunct('!') && !ispunct('a'), "ispunct");
}

static int cmp_int(const void *a, const void *b) { int x=*(const int*)a, y=*(const int*)b; return x<y?-1:(x>y?1:0); }

static void t_stdlib(void)
{
    CHK_INT(atoi("42"), 42, "atoi");
    CHK_INT(atoi("  -7x"), -7, "atoi leading ws + trailing junk");
    CHK_INT(atol("100000"), 100000L, "atol");
    char *end;
    CHK_INT(strtol("ff", &end, 16), 255, "strtol hex");
    CHK_INT(strtol("0x1A", &end, 0), 26, "strtol auto 0x");
    CHK_INT(strtol("  12 ", &end, 10), 12, "strtol ws"); CHK(*end==' ', "strtol endptr");
    CHK_INT(strtol("-2147483648", &end, 10), -2147483648L, "strtol min32");
    errno = 0; (void)strtol("99999999999999999999", &end, 10); CHK_INT(errno, ERANGE, "strtol overflow ERANGE");
    CHK_INT(strtoul("4294967295", &end, 10), 4294967295UL, "strtoul");
    CHK_INT(abs(-5), 5, "abs"); CHK_INT(labs(-7L), 7L, "labs");
    int arr[] = {5,2,9,1,7,3};
    qsort(arr, 6, sizeof(int), cmp_int);
    CHK(arr[0]==1 && arr[5]==9, "qsort");
    int key = 7, *found = bsearch(&key, arr, 6, sizeof(int), cmp_int);
    CHK(found && *found==7, "bsearch hit");
    key = 8; CHK(bsearch(&key, arr, 6, sizeof(int), cmp_int) == 0, "bsearch miss");
}

static void t_stdio(void)
{
    char b[64];
    CHK_INT(snprintf(b, sizeof b, "%d", 42), 2, "snprintf %d ret");  CHK_STR(b, "42", "snprintf %d");
    snprintf(b, sizeof b, "%05d", 42); CHK_STR(b, "00042", "snprintf width0");
    snprintf(b, sizeof b, "%x %X", 255, 255); CHK_STR(b, "ff FF", "snprintf hex");
    snprintf(b, sizeof b, "%s/%c", "ab", 'Z'); CHK_STR(b, "ab/Z", "snprintf s c");
    snprintf(b, sizeof b, "%u", 4000000000U); CHK_STR(b, "4000000000", "snprintf u");
    snprintf(b, sizeof b, "%ld", -1234567890L); CHK_STR(b, "-1234567890", "snprintf ld");
    snprintf(b, sizeof b, "%+d %-5d|", 3, 7); CHK_STR(b, "+3 7    |", "snprintf flags");
    /* truncation: returns the would-be length, writes <= size-1 + NUL */
    int n = snprintf(b, 4, "%d", 123456); CHK_INT(n, 6, "snprintf trunc ret"); CHK_STR(b, "123", "snprintf trunc out");
    snprintf(b, sizeof b, "%.2f", 3.14159); CHK_STR(b, "3.14", "snprintf %.2f");
    snprintf(b, sizeof b, "%g", 100000.0); CHK_STR(b, "100000", "snprintf %g");
}

int main(void)
{
    t_string();
    t_ctype();
    t_stdlib();
    t_stdio();
    if (fails == 0) printf("LIBC_OK %d/%d\n", checks - fails, checks);
    else            printf("LIBC_FAIL %d/%d (%d failed)\n", checks - fails, checks, fails);
    return fails ? 1 : 0;
}
