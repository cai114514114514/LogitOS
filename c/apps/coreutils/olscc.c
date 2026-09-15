/* The guest and host compile exactly the same bounded OLS-IR language. Output
 * is versioned OLS v1 bytecode, validated again by the runtime before use. */
#include "openlogit_3d.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv)
{
    if(argc!=3){fprintf(stderr,"usage: olscc input.ols output.olsb\n");return 2;}
    FILE *f=fopen(argv[1],"rb");if(!f){perror(argv[1]);return 1;}
    char *s=malloc(131073);if(!s){fclose(f);return 1;}
    size_t n=fread(s,1,131073,f);int error=ferror(f);fclose(f);
    if(error||n>131072){fprintf(stderr,"olscc: cannot read program (limit 128 KiB)\n");free(s);return 1;}
    struct ol_shader_program *p=malloc(sizeof *p);struct ol_shader_error e;
    if(!p){free(s);return 1;}
    int rc=ol_shader_compile(s,n,p,&e);free(s);
    if(rc){fprintf(stderr,"%s:%u: %s\n",argv[1],e.line,e.message);free(p);return 1;}
    f=fopen(argv[2],"wb");if(!f){perror(argv[2]);free(p);return 1;}
    error=fwrite(p,1,sizeof *p,f)!=sizeof *p;
    if(fclose(f))error=1;
    if(!error)printf("OLS-IR v1: %u instructions, %s stage\n",p->count,p->stage==OLS_VERTEX?"vertex":"pixel");
    free(p);return error?1:0;
}
