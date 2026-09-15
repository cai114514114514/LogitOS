/* Standalone SVG has no browser DOM or LibCSS dependency. Reuse its existing
 * solid-paint value parsers, and compile only the simple selector subset that
 * can be matched exactly here. Unsupported selectors/at-rules are skipped as
 * whole rules, never reduced to a matching prefix. All spans borrow the input
 * bytes for this synchronous decode; scene-owned rules are freed on return. */
#define SV_CSS_RULES 512
#define SV_CSS_PROPS 11
static const char *const sv_css_names[SV_CSS_PROPS]={
    "color","fill","stroke","fill-opacity","stroke-opacity","opacity",
    "fill-rule","clip-rule","stroke-width","display","visibility"
};
static const char *const sv_css_initial[SV_CSS_PROPS]={
    "black","black","none","1","1","1","nonzero","nonzero","1","inline","visible"
};
struct sv_css_values { const uint8_t *v[SV_CSS_PROPS];int n[SV_CSS_PROPS];unsigned important; };
struct sv_css_rule { const uint8_t *selector;int len;struct sv_css_values values; };
struct sv_css_winners { struct sv_css_values values;unsigned specificity[SV_CSS_PROPS]; };
static int sv_css_n(const char *p){int n=0;while(p[n])n++;return n;}
static int sv_css_ident(int c,int first)
{return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='-'||c>=128||(!first&&c>='0'&&c<='9');}
static const uint8_t *sv_css_space(const uint8_t *p,const uint8_t *end)
{
    for(;;) {
        while(p<end&&is_ws(*p))p++;
        if(end-p>=2&&p[0]=='/'&&p[1]=='*') {
            p+=2;while(end-p>=2&&!(p[0]=='*'&&p[1]=='/'))p++;
            if(end-p<2)return end;p+=2;continue;
        }
        return p;
    }
}
static int sv_css_keyword(const uint8_t *v,int n)
{
    if(sv_eq(v,n,"inherit"))return 1;
    if(sv_eq(v,n,"initial"))return 2;
    if(sv_eq(v,n,"unset"))return 3;
    return 0;
}
/* The raster literal parser is intentionally permissive. Cascade selection
 * needs complete supported tokens so an invalid later value cannot win. */
static int sv_css_color_valid(int prop,const uint8_t *v,int n)
{
    uint8_t decoded[256];int dn=sv_unescape(v,n,decoded,sizeof decoded);
    if(dn<0)return 0;const uint8_t *p=decoded;sv_trim(&p,&dn);
    if(!dn||(prop==0&&sv_eq(p,dn,"none")))return 0;
    if(*p=='#') {
        int count=dn-1;if(count!=3&&count!=4&&count!=6&&count!=8)return 0;
        for(int i=1;i<dn;i++)if(hexval(p[i])<0)return 0;
    } else if(dn>=4&&p[0]=='r'&&p[1]=='g'&&p[2]=='b'&&(p[3]=='('||(dn>=5&&p[3]=='a'&&p[4]=='('))) {
        int alpha=p[3]=='a',comma=-1;const uint8_t *q=p+(alpha?5:4),*end=p+dn;
        for(int i=0;i<3+alpha;i++) {
            while(q<end&&is_ws(*q))q++;
            int number;if(pnum_fx(&q,end,&number))return 0;
            if(q<end&&*q=='%'){if(i==3)return 0;q++;}
            const uint8_t *after=q;while(q<end&&is_ws(*q))q++;
            if(i<2+alpha) {
                int iscomma=q<end&&*q==',';
                if(comma<0)comma=iscomma;
                if(comma!=iscomma||(!iscomma&&after==q))return 0;
                if(iscomma)q++;
            }
        }
        if(q==end||*q++!=')'||q!=end)return 0;
    }
    struct paint paint={0};int current=0;return sv_literal(v,n,&paint,&current);
}
static int sv_css_valid(int prop,const uint8_t *v,int n)
{
    if(!n)return 0;if(sv_css_keyword(v,n))return 1;
    if(prop<3)return sv_css_color_valid(prop,v,n);
    if(prop<6)return sv_opacity(v,n,-1)>=0;
    if(prop<8)return sv_eq(v,n,"nonzero")||sv_eq(v,n,"evenodd");
    if(prop==8){int w;const uint8_t *p=v;return !pnum_fx(&p,v+n,&w)&&w>=0&&(p==v+n||str_eq(p,(int)(v+n-p),"px"));}
    if(prop==9)return sv_eq(v,n,"none")||sv_eq(v,n,"inline")||sv_eq(v,n,"block");
    return sv_eq(v,n,"visible")||sv_eq(v,n,"hidden")||sv_eq(v,n,"collapse");
}
static void sv_css_declarations(const uint8_t *p,int len,struct sv_css_values *out)
{
    const uint8_t *end=p+len,*key,*value;int kn,vn,important;
    while(sv_decl_next(&p,end,&key,&kn,&value,&vn,&important)) {
        for(int i=0;i<SV_CSS_PROPS;i++)if(sv_eq(key,kn,sv_css_names[i])) {
            if(sv_css_valid(i,value,vn)&&(!(out->important&(1u<<i))||important)) {
                out->v[i]=value;out->n[i]=vn;out->important&=~(1u<<i);
                if(important)out->important|=1u<<i;
            }
            break;
        }
    }
}
static int sv_css_attr(const struct tag *t,const char *name,const uint8_t *word,int n,int token)
{
    const uint8_t *raw;int rn;uint8_t decoded[1024];
    if(attr_get(t,name,&raw,&rn))return 0;
    int dn=sv_unescape(raw,rn,decoded,sizeof decoded);if(dn<0)return 0;
    int p=0;do {
        while(p<dn&&is_ws(decoded[p]))p++;
        int a=p;if(token)while(p<dn&&!is_ws(decoded[p]))p++;else p=dn;
        if(p-a==n){int j=0;while(j<n&&decoded[a+j]==word[j])j++;if(j==n)return 1;}
    }while(token&&p<dn);
    return 0;
}
static const uint8_t *sv_css_comments(const uint8_t *p,const uint8_t *end)
{
    while(end-p>=2&&p[0]=='/'&&p[1]=='*') {
        p+=2;while(end-p>=2&&!(p[0]=='*'&&p[1]=='/'))p++;
        if(end-p<2)return end;p+=2;
    }
    return p;
}
/* Validate the complete list even after a match; .ok:hover cannot become .ok,
 * and an invalid arm invalidates an ordinary (non-forgiving) selector list. */
static int sv_css_selector(const uint8_t *p,int len,const struct tag *t,unsigned *specificity)
{
    const uint8_t *end=p+len;int any=0;unsigned best=0;
    for(;;) {
        p=sv_css_space(p,end);if(p==end)return -1;
        int matched=1,parts=0;unsigned spec=0;
        if(*p=='*'){p++;parts++;}
        else if(sv_css_ident(*p,1)) {
            const uint8_t *a=p++;while(p<end&&sv_css_ident(*p,0))p++;
            int n=(int)(p-a);parts++;spec++;
            if(t){if(n!=t->nlen)matched=0;else for(int j=0;j<n;j++)if(a[j]!=t->name[j])matched=0;}
        }
        p=sv_css_comments(p,end);
        while(p<end&&(*p=='.'||*p=='#')) {
            int id=*p++=='#';const uint8_t *a=p;
            if(p==end||!sv_css_ident(*p,1))return -1;
            p++;while(p<end&&sv_css_ident(*p,0))p++;
            if(++parts>32)return -1;
            spec+=id?65536:256;
            if(t&&!sv_css_attr(t,id?"id":"class",a,(int)(p-a),!id))matched=0;
            p=sv_css_comments(p,end);
        }
        if(!parts)return -1;
        p=sv_css_space(p,end);
        if(p<end&&*p!=',')return -1;
        if(matched){any=1;if(spec>best)best=spec;}
        if(p==end){*specificity=best;return any;}
        p++; /* The next iteration refuses an empty last arm. */
    }
}
static const uint8_t *sv_css_block_end(const uint8_t *p,const uint8_t *end)
{
    int depth=1,quote=0;
    for(;p<end;p++) {
        int c=*p;
        if(quote){if(c=='\\'&&p+1<end)p++;else if(c==quote)quote=0;continue;}
        if(c=='\''||c=='"'){quote=c;continue;}
        if(end-p>=2&&c=='/'&&p[1]=='*'){p=sv_css_space(p,end);if(p==end)return end;p--;continue;}
        if(c=='{')depth++;else if(c=='}'&&!--depth)return p;
    }
    return end;
}
static void sv_css_sheet(struct sv_scene *s,const uint8_t *p,int len)
{
    const uint8_t *end=p+len;
    while(p<end&&!s->failed) {
        p=sv_css_space(p,end);if(p==end)break;
        /* XML CDATA and historical CSS comment wrappers are transport syntax,
         * not selectors. Their contents still enter the same rule parser. */
        if(end-p>=9&&str_eq(p,9,"<![CDATA[")){p+=9;continue;}
        if(end-p>=3&&(str_eq(p,3,"]]>")||str_eq(p,3,"-->"))){p+=3;continue;}
        if(end-p>=4&&str_eq(p,4,"<!--")){p+=4;continue;}
        const uint8_t *a=p;int at=*p=='@',quote=0,par=0;
        for(;p<end;p++) {
            int c=*p;
            if(quote){if(c=='\\'&&p+1<end)p++;else if(c==quote)quote=0;continue;}
            if(c=='\''||c=='"'){quote=c;continue;}
            if(end-p>=2&&c=='/'&&p[1]=='*'){p=sv_css_space(p,end);if(p==end)break;p--;continue;}
            if(c=='(')par++;else if(c==')'&&par)par--;
            else if(!par&&(c=='{'||c==';'))break;
        }
        if(p==end)break;
        if(*p==';'){p++;continue;}
        int sn=(int)(p-a);const uint8_t *body=++p,*stop=sv_css_block_end(body,end);
        p=stop<end?stop+1:end;
        if(at)continue;
        unsigned specificity;if(sv_css_selector(a,sn,0,&specificity)<0)continue;
        struct sv_css_values values={0};sv_css_declarations(body,(int)(stop-body),&values);
        int any=0;for(int i=0;i<SV_CSS_PROPS;i++)if(values.v[i])any=1;
        if(!any)continue;
        if(s->css_count==SV_CSS_RULES){s->failed=1;break;}
        s->css[s->css_count++]=(struct sv_css_rule){a,sn,values};
    }
}
static int sv_css_compile(struct sv_scene *s)
{
#ifndef SVG_SCENE_NO_STYLESHEET
    for(int i=0;i<s->count;i++) {
        const struct sv_node *node=&s->nodes[i];const struct tag *t=&node->t;
        if(!tag_is(t,"style")||!node->text||!node->textlen)continue;
        const uint8_t *v;int n;
        if(!attr_get(t,"type",&v,&n)){sv_trim(&v,&n);if(n&&!sv_eq(v,n,"text/css"))continue;}
        if(!attr_get(t,"media",&v,&n)){sv_trim(&v,&n);if(n&&!sv_eq(v,n,"all"))continue;}
        if(!s->css){s->css=kmalloc(sizeof(*s->css)*SV_CSS_RULES);if(!s->css)return 0;}
        sv_css_sheet(s,node->text,node->textlen);if(s->failed)return 0;
    }
#else
    (void)s;
#endif
    return 1;
}
static void sv_css_merge(struct sv_css_winners *out,const struct sv_css_values *v,unsigned spec)
{
    for(int i=0;i<SV_CSS_PROPS;i++)if(v->v[i]) {
        unsigned imp=!!(v->important&(1u<<i)),oldimp=!!(out->values.important&(1u<<i));
        if(!out->values.v[i]||imp>oldimp||(imp==oldimp&&spec>=out->specificity[i])) {
            out->values.v[i]=v->v[i];out->values.n[i]=v->n[i];out->specificity[i]=spec;
            out->values.important&=~(1u<<i);if(imp)out->values.important|=1u<<i;
        }
    }
}
static void sv_css_cascade(struct sv_scene *s,int idx,struct sv_css_values *result)
{
    const struct tag *t=&s->nodes[idx].t;struct sv_css_winners out={0};
    for(int i=0;i<SV_CSS_PROPS;i++) {
        const uint8_t *v;int n;
        if(!attr_get(t,sv_css_names[i],&v,&n)){sv_trim(&v,&n);if(sv_css_valid(i,v,n)){out.values.v[i]=v;out.values.n[i]=n;}}
    }
    for(int i=0;i<s->css_count;i++) {
        unsigned spec;struct sv_css_rule *r=&s->css[i];
        if(sv_css_selector(r->selector,r->len,t,&spec)>0)sv_css_merge(&out,&r->values,spec+1);
    }
    const uint8_t *inline_css;int n;
    if(!attr_get(t,"style",&inline_css,&n)) {
        struct sv_css_values values={0};sv_css_declarations(inline_css,n,&values);
        sv_css_merge(&out,&values,1u<<24);
    }
    *result=out.values;
}
static int sv_css_get(const struct sv_css_values *values,const char *name,const uint8_t **v,int *n)
{
    for(int i=0;i<SV_CSS_PROPS;i++)if(str_eq((const uint8_t *)name,sv_css_n(name),sv_css_names[i])) {
        if(!values->v[i])return 0;*v=values->v[i];*n=values->n[i];
        int key=sv_css_keyword(*v,*n);
        if(key==1)return 0;
        if(key==3&&i!=5&&i!=9)return 0; /* all other supported properties inherit */
        if(key==2||key==3){*v=(const uint8_t *)sv_css_initial[i];*n=sv_css_n(sv_css_initial[i]);}
        return 1;
    }
    return 0;
}
