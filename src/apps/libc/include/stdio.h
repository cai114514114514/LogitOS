#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>

typedef struct _FILE FILE;
extern FILE *stdin, *stdout, *stderr;

#define EOF (-1)
#define BUFSIZ 4096

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
int   vprintf(const char *, va_list);
int   vfprintf(FILE *, const char *, va_list);
int   vsnprintf(char *, size_t, const char *, va_list);
int   vsprintf(char *, const char *, va_list);
int   putchar(int);
int   puts(const char *);
int   fputc(int, FILE *);
int   fputs(const char *, FILE *);
int   fflush(FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
#define putc(c, f) fputc((c), (f))

/* input */
int   fgetc(FILE *);
int   getc(FILE *);
int   getchar(void);
int   ungetc(int, FILE *);
char *fgets(char *, int, FILE *);
size_t fread(void *, size_t, size_t, FILE *);

/* open / close / position / status */
FILE *fopen(const char *, const char *);
FILE *fdopen(int, const char *);
int   fclose(FILE *);
int   fseek(FILE *, long, int);
long  ftell(FILE *);
void  rewind(FILE *);
int   feof(FILE *);
int   ferror(FILE *);
void  clearerr(FILE *);
int   fileno(FILE *);
int   remove(const char *);

#endif
