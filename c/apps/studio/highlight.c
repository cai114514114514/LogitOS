/* SPDX-License-Identifier: MIT */
#include "highlight.h"
#include "../as/editor/completion.h"
static int alpha(unsigned char c)
{return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c>=128;}
static int digit(char c){return c>='0'&&c<='9';}
void st_highlight(const char *s,int n,unsigned char *out)
{
    for(int i=0;i<n;){
        int start=i,ink=ST_INK_TEXT;char c=s[i++];
        if(c=='#'){while(i<n&&s[i]!='\n')i++;ink=ST_INK_COMMENT;}
        else if(c=='\''||c=='"'){
            while(i<n){char t=s[i++];if(t=='\\'&&i<n)i++;else if(t==c)break;}
            ink=ST_INK_STRING;
        }else if(digit(c)){
            while(i<n&&(alpha((unsigned char)s[i])||digit(s[i])||s[i]=='.'||
                  ((s[i]=='+'||s[i]=='-')&&(s[i-1]=='e'||s[i-1]=='E'))))i++;
            ink=ST_INK_NUMBER;
        }else if(alpha((unsigned char)c)){
            while(i<n&&(alpha((unsigned char)s[i])||digit(s[i])))i++;
            int kind=as_word_kind(s+start,i-start),next=i;
            while(next<n&&(s[next]==' '||s[next]=='\t'))next++;
            ink=kind==CMP_KEYWORD?ST_INK_KEYWORD:
                (kind==CMP_BUILTIN||(next<n&&s[next]=='('))?ST_INK_CALL:ST_INK_TEXT;
        }else if(c!=' '&&c!='\t'&&c!='\r'&&c!='\n')ink=ST_INK_OPERATOR;
        for(int j=start;j<i;j++)out[j]=(unsigned char)ink;
    }
}
