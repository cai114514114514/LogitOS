/* Rename every mini-libc symbol so ours and glibc's can live in one process.
 *
 * This is the crypto suite's `make test-crypto-diff` idiom applied to a libc:
 * the strongest available statement about a C library is not "it has the
 * function" but "it agrees with a reference implementation on adversarial
 * input". To compare, both must be linked into the same binary -- hence the
 * renaming. Force-included (-include) into the mini-libc translation units by
 * the test-libc-diff rule; it renames declarations and definitions together, so
 * the sources are compiled completely unmodified.
 *
 * The second group is the other direction: symbols mini-libc IMPORTS (the
 * allocator, the raw file syscalls). Those are renamed too, and the driver
 * defines them as thin forwards to glibc -- which is what lets the host test
 * exercise fopen/fread/fprintf and not just the pure string functions. */
#ifndef LIBC_RENAME_H
#define LIBC_RENAME_H

/* --- state ------------------------------------------------------------- */
#define errno            mini_errno

/* Negative control. A test suite that has never failed is not known to be able
 * to fail. Under -DLIBC_SABOTAGE the real strtod is renamed one step further
 * out of the way and tests/unit/libc_sabotage.c supplies a mini_strtod that
 * perturbs one result in a thousand by a single ulp -- the smallest error a
 * libc can make and precisely the class this suite exists to catch. The
 * `test-libc-diff` rule builds BOTH binaries and requires the clean one to pass
 * and the sabotaged one to fail; if the sabotaged one ever passes, the suite is
 * not measuring what it claims to. */
#ifdef LIBC_SABOTAGE
#define strtod           mini_strtod_real
#endif

/* --- imported by mini-libc, supplied by the driver ---------------------- */
#define malloc           mini_malloc
#define free             mini_free
#define realloc          mini_realloc
#define calloc           mini_calloc
#define malloc_usable_size mini_malloc_usable_size
#define read             mini_read
#define write            mini_write
#define open             mini_open
#define close            mini_close
#define lseek            mini_lseek
#define unlink           mini_unlink
#define fsync            mini_fsync
#define isatty           mini_isatty
#define rename           mini_rename
#define mkdir            mini_mkdir
#define getcwd           mini_getcwd
#define _exit            mini__exit

/* --- string.h ---------------------------------------------------------- */
#define memcpy           mini_memcpy
#define memmove          mini_memmove
#define memset           mini_memset
#define memcmp           mini_memcmp
#define memchr           mini_memchr
#define memrchr          mini_memrchr
#define memccpy          mini_memccpy
#define memmem           mini_memmem
#define mempcpy          mini_mempcpy
#define strlen           mini_strlen
#define strnlen          mini_strnlen
#define strcmp           mini_strcmp
#define strncmp          mini_strncmp
#define strcoll          mini_strcoll
#define strxfrm          mini_strxfrm
#define strcpy           mini_strcpy
#define strncpy          mini_strncpy
#define stpcpy           mini_stpcpy
#define stpncpy          mini_stpncpy
#define strcat           mini_strcat
#define strncat          mini_strncat
#define strchr           mini_strchr
#define strchrnul        mini_strchrnul
#define strrchr          mini_strrchr
#define strstr           mini_strstr
#define strcasestr       mini_strcasestr
#define strpbrk          mini_strpbrk
#define strtok           mini_strtok
#define strtok_r         mini_strtok_r
#define strspn           mini_strspn
#define strcspn          mini_strcspn
#define strdup           mini_strdup
#define strndup          mini_strndup
#define strerror         mini_strerror
#define strerror_r       mini_strerror_r
#define strsignal        mini_strsignal
#define strcasecmp       mini_strcasecmp
#define strncasecmp      mini_strncasecmp
#define strlcpy          mini_strlcpy
#define strlcat          mini_strlcat
#define strsep           mini_strsep
#define strverscmp       mini_strverscmp
#define bcmp             mini_bcmp
#define bcopy            mini_bcopy
#define bzero            mini_bzero
#define index            mini_index
#define rindex           mini_rindex
#define ffs              mini_ffs
#define ffsl             mini_ffsl
#define ffsll            mini_ffsll

/* --- stdlib.h ---------------------------------------------------------- */
#define atoi             mini_atoi
#define atol             mini_atol
#define atoll            mini_atoll
#define atof             mini_atof
#ifndef LIBC_SABOTAGE
#define strtod           mini_strtod
#endif
#define strtof           mini_strtof
#define strtold          mini_strtold
#define strtol           mini_strtol
#define strtoul          mini_strtoul
#define strtoll          mini_strtoll
#define strtoull         mini_strtoull
#define strtoimax        mini_strtoimax
#define strtoumax        mini_strtoumax
#define abs              mini_abs
#define labs             mini_labs
#define llabs            mini_llabs
#define imaxabs          mini_imaxabs
#define div              mini_div
#define ldiv             mini_ldiv
#define lldiv            mini_lldiv
#define imaxdiv          mini_imaxdiv
#define qsort            mini_qsort
#define qsort_r          mini_qsort_r
#define bsearch          mini_bsearch
#define rand             mini_rand
#define srand            mini_srand
#define rand_r           mini_rand_r
#define random           mini_random
#define srandom          mini_srandom
#define getenv           mini_getenv
#define setenv           mini_setenv
#define unsetenv         mini_unsetenv
#define putenv           mini_putenv
#define clearenv         mini_clearenv
#define environ          mini_environ
#define exit             mini_exit
#define _Exit            mini__Exit
#define quick_exit       mini_quick_exit
#define at_quick_exit    mini_at_quick_exit
#define atexit           mini_atexit
#define abort            mini_abort
#define system           mini_system
#define aligned_alloc    mini_aligned_alloc
#define posix_memalign   mini_posix_memalign
#define mblen            mini_mblen
#define mbtowc           mini_mbtowc
#define wctomb           mini_wctomb
#define mbstowcs         mini_mbstowcs
#define wcstombs         mini_wcstombs
#define mkstemp          mini_mkstemp
#define mktemp           mini_mktemp

/* --- stdio.h ----------------------------------------------------------- */
#define FILE             mini_FILE
#define _FILE            mini__FILE
#define stdin            mini_stdin
#define stdout           mini_stdout
#define stderr           mini_stderr
#define printf           mini_printf
#define fprintf          mini_fprintf
#define sprintf          mini_sprintf
#define snprintf         mini_snprintf
#define asprintf         mini_asprintf
#define dprintf          mini_dprintf
#define vprintf          mini_vprintf
#define vfprintf         mini_vfprintf
#define vsprintf         mini_vsprintf
#define vsnprintf        mini_vsnprintf
#define vasprintf        mini_vasprintf
#define vdprintf         mini_vdprintf
#define scanf            mini_scanf
#define fscanf           mini_fscanf
#define sscanf           mini_sscanf
#define vscanf           mini_vscanf
#define vfscanf          mini_vfscanf
#define vsscanf          mini_vsscanf
#define putchar          mini_putchar
#define puts             mini_puts
#define fputc            mini_fputc
#define putc             mini_putc
#define fputs            mini_fputs
#define fflush           mini_fflush
#define fwrite           mini_fwrite
#define fgetc            mini_fgetc
#define getc             mini_getc
#define getchar          mini_getchar
#define ungetc           mini_ungetc
#define fgets            mini_fgets
#define fread            mini_fread
#define getline          mini_getline
#define getdelim         mini_getdelim
#define fopen            mini_fopen
#define freopen          mini_freopen
#define fdopen           mini_fdopen
#define fclose           mini_fclose
#define fseek            mini_fseek
#define fseeko           mini_fseeko
#define ftell            mini_ftell
#define ftello           mini_ftello
#define fgetpos          mini_fgetpos
#define fsetpos          mini_fsetpos
#define fpos_t           mini_fpos_t
#define rewind           mini_rewind
#define feof             mini_feof
#define ferror           mini_ferror
#define clearerr         mini_clearerr
#define fileno           mini_fileno
#define remove           mini_remove
#define perror           mini_perror
#define setbuf           mini_setbuf
#define setvbuf          mini_setvbuf
#define setlinebuf       mini_setlinebuf
#define tmpfile          mini_tmpfile
#define tmpnam           mini_tmpnam
#define fwide            mini_fwide

/* --- time.h ------------------------------------------------------------ */
#define time             mini_time
#define clock            mini_clock
#define clock_gettime    mini_clock_gettime
#define gettimeofday     mini_gettimeofday
#define difftime         mini_difftime
#define mktime           mini_mktime
#define timegm           mini_timegm
#define gmtime           mini_gmtime
#define gmtime_r         mini_gmtime_r
#define localtime        mini_localtime
#define localtime_r      mini_localtime_r
#define asctime          mini_asctime
#define asctime_r        mini_asctime_r
#define ctime            mini_ctime
#define ctime_r          mini_ctime_r
#define strftime         mini_strftime
#define wcsftime         mini_wcsftime
#define timespec_get     mini_timespec_get
#define nanosleep        mini_nanosleep
#define tzset            mini_tzset
#define tzname           mini_tzname
#define timezone         mini_timezone
#define daylight         mini_daylight
#define sleep            mini_sleep
#define usleep           mini_usleep

/* --- locale.h / signal.h ----------------------------------------------- */
#define setlocale        mini_setlocale
#define localeconv       mini_localeconv
#define lconv            mini_lconv
#define signal           mini_signal
#define raise            mini_raise
#define sigaction        mini_sigaction
#define sigemptyset      mini_sigemptyset
#define sigfillset       mini_sigfillset
#define sigaddset        mini_sigaddset
#define sigdelset        mini_sigdelset
#define sigismember      mini_sigismember
#define sigprocmask      mini_sigprocmask
#define kill             mini_kill

/* --- wchar.h / wctype.h ------------------------------------------------ */
#define wcslen           mini_wcslen
#define wcsnlen          mini_wcsnlen
#define wcscpy           mini_wcscpy
#define wcpcpy           mini_wcpcpy
#define wcsncpy          mini_wcsncpy
#define wcscat           mini_wcscat
#define wcsncat          mini_wcsncat
#define wcscmp           mini_wcscmp
#define wcsncmp          mini_wcsncmp
#define wcscasecmp       mini_wcscasecmp
#define wcsncasecmp      mini_wcsncasecmp
#define wcscoll          mini_wcscoll
#define wcsxfrm          mini_wcsxfrm
#define wcschr           mini_wcschr
#define wcsrchr          mini_wcsrchr
#define wcsstr           mini_wcsstr
#define wcspbrk          mini_wcspbrk
#define wcsspn           mini_wcsspn
#define wcscspn          mini_wcscspn
#define wcstok           mini_wcstok
#define wcsdup           mini_wcsdup
#define wmemcpy          mini_wmemcpy
#define wmemmove         mini_wmemmove
#define wmemset          mini_wmemset
#define wmemcmp          mini_wmemcmp
#define wmemchr          mini_wmemchr
#define wcstod           mini_wcstod
#define wcstof           mini_wcstof
#define wcstold          mini_wcstold
#define wcstol           mini_wcstol
#define wcstoul          mini_wcstoul
#define wcstoll          mini_wcstoll
#define wcstoull         mini_wcstoull
#define wcstoimax        mini_wcstoimax
#define wcstoumax        mini_wcstoumax
#define btowc            mini_btowc
#define wctob            mini_wctob
#define mbsinit          mini_mbsinit
#define mbrlen           mini_mbrlen
#define mbrtowc          mini_mbrtowc
#define wcrtomb          mini_wcrtomb
#define mbsrtowcs        mini_mbsrtowcs
#define wcsrtombs        mini_wcsrtombs
#define mbstate_t        mini_mbstate_t
#define swprintf         mini_swprintf
#define vswprintf        mini_vswprintf
#define iswalnum         mini_iswalnum
#define iswalpha         mini_iswalpha
#define iswblank         mini_iswblank
#define iswcntrl         mini_iswcntrl
#define iswdigit         mini_iswdigit
#define iswgraph         mini_iswgraph
#define iswlower         mini_iswlower
#define iswprint         mini_iswprint
#define iswpunct         mini_iswpunct
#define iswspace         mini_iswspace
#define iswupper         mini_iswupper
#define iswxdigit        mini_iswxdigit
#define iswctype         mini_iswctype
#define wctype           mini_wctype
#define towlower         mini_towlower
#define towupper         mini_towupper
#define towctrans        mini_towctrans
#define wctrans          mini_wctrans
#define wctype_t         mini_wctype_t
#define wctrans_t        mini_wctrans_t

/* --- fenv.h ------------------------------------------------------------ */
#define fesetround       mini_fesetround
#define fegetround       mini_fegetround
#define feclearexcept    mini_feclearexcept
#define fetestexcept     mini_fetestexcept
#define feraiseexcept    mini_feraiseexcept
#define fegetenv         mini_fegetenv
#define fesetenv         mini_fesetenv
#define feholdexcept     mini_feholdexcept
#define feupdateenv      mini_feupdateenv
#define fegetexceptflag  mini_fegetexceptflag
#define fesetexceptflag  mini_fesetexceptflag

/* --- math (only what mini-libc itself defines) ------------------------- */
#define sqrtf            mini_sqrtf
#define fabsf            mini_fabsf
#define floorf           mini_floorf
#define ceilf            mini_ceilf
#define truncf           mini_truncf
#define roundf           mini_roundf
#define nearbyintf       mini_nearbyintf
#define rintf            mini_rintf
#define copysignf        mini_copysignf
#define fmodf            mini_fmodf
#define fminf            mini_fminf
#define fmaxf            mini_fmaxf
#define fdimf            mini_fdimf
#define frexpf           mini_frexpf
#define ldexpf           mini_ldexpf
#define scalbnf          mini_scalbnf
#define modff            mini_modff
#define lroundf          mini_lroundf
#define llroundf         mini_llroundf
#define lrintf           mini_lrintf
#define llrintf          mini_llrintf
#define nanf             mini_nanf
#define hypotf           mini_hypotf


/* resource.c. NOT cosmetic: an un-renamed symbol of ours does not merely fail
 * to be tested, it OVERRIDES glibc's for the whole process -- and ASan calls
 * setrlimit() from DisableCoreDumperIfNecessary() during its own init, before
 * main, before anything of ours is ready. The result was a segfault inside
 * __asan_init with no output at all, which reads like a broken test binary
 * rather than a missing #define.
 *
 * That is the mirror image of the trap already documented at the top of
 * tests/libc.mk: there, a MISSING implementation silently falls back to
 * glibc's; here, a PRESENT one silently replaces it. */
#define getrlimit        mini_getrlimit
#define setrlimit        mini_setrlimit
#define getrusage        mini_getrusage

#endif /* LIBC_RENAME_H */
