#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>

typedef struct _FILE FILE;
typedef long fpos_t;
extern FILE *stdin, *stdout, *stderr;

#ifndef _SSIZE_T_DECLARED
#define _SSIZE_T_DECLARED
typedef long ssize_t;
#endif
#ifndef _OFF_T_DECLARED
#define _OFF_T_DECLARED
typedef long off_t;
#endif

#define EOF (-1)
#define BUFSIZ 4096
#define FILENAME_MAX 256
#define FOPEN_MAX 32
#define TMP_MAX 32768
#define L_tmpnam 32

/* setvbuf modes */
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* output */
int   printf(const char *, ...);
int   fprintf(FILE *, const char *, ...);
int   sprintf(char *, const char *, ...);
int   snprintf(char *, size_t, const char *, ...);
int   asprintf(char **, const char *, ...);
int   dprintf(int, const char *, ...);
int   vprintf(const char *, va_list);
int   vfprintf(FILE *, const char *, va_list);
int   vsnprintf(char *, size_t, const char *, va_list);
int   vsprintf(char *, const char *, va_list);
int   vasprintf(char **, const char *, va_list);
int   vdprintf(int, const char *, va_list);
int   putchar(int);
int   puts(const char *);
int   fputc(int, FILE *);
int   putc(int, FILE *);
int   fputs(const char *, FILE *);
int   fflush(FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
void  perror(const char *);

/* input */
int   fgetc(FILE *);
int   getc(FILE *);
int   getchar(void);
int   ungetc(int, FILE *);
char *fgets(char *, int, FILE *);
size_t fread(void *, size_t, size_t, FILE *);
ssize_t getline(char **, size_t *, FILE *);
ssize_t getdelim(char **, size_t *, int, FILE *);
int   scanf(const char *, ...);
int   fscanf(FILE *, const char *, ...);
int   sscanf(const char *, const char *, ...);
int   vscanf(const char *, va_list);
int   vfscanf(FILE *, const char *, va_list);
int   vsscanf(const char *, const char *, va_list);

/* open / close / position / status */
FILE *fopen(const char *, const char *);
FILE *freopen(const char *, const char *, FILE *);
FILE *fdopen(int, const char *);
FILE *tmpfile(void);
char *tmpnam(char *);
int   fclose(FILE *);
int   fseek(FILE *, long, int);
int   fseeko(FILE *, off_t, int);
long  ftell(FILE *);
off_t ftello(FILE *);
int   fgetpos(FILE *, fpos_t *);
int   fsetpos(FILE *, const fpos_t *);
void  rewind(FILE *);
int   feof(FILE *);
int   ferror(FILE *);
void  clearerr(FILE *);
int   fileno(FILE *);
int   remove(const char *);
int   rename(const char *, const char *);
int   setvbuf(FILE *, char *, int, size_t);
void  setbuf(FILE *, char *);
void  setlinebuf(FILE *);
int   fwide(FILE *, int);

#endif
