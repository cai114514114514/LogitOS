/* SPDX-License-Identifier: MIT */
#include "internal.h"

int st_line_start(const StDocument *d,int line)
{int p=0;while(line--&&p<d->length){while(p<d->length&&d->text[p]!='\n')p++;if(p<d->length)p++;}return p;}

int st_line_end(const StDocument *d,int p){while(p<d->length&&d->text[p]!='\n')p++;return p;}

int st_caret_line(const StDocument *d){int row=0;for(int i=0;i<d->caret;i++)if(d->text[i]=='\n')row++;return row;}
