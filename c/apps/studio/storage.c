/* SPDX-License-Identifier: MIT */
#include "storage.h"
static int st_write_all(int fd,const void *data,size_t bytes)
{const char *p=data;while(bytes){ssize_t n=write(fd,p,bytes);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;bytes-=(size_t)n;}return 0;}
static int st_read_all(int fd,void *data,size_t bytes)
{char *p=data;while(bytes){ssize_t n=read(fd,p,bytes);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;bytes-=(size_t)n;}return 0;}
int st_read_file(const char *path,char **text,int *length,int *exists)
{
    *text=NULL;*length=0;*exists=0;
    int fd=open(path,O_RDONLY);if(fd<0){if(errno!=ENOENT)return -1;*text=calloc(1,1);return *text?0:-1;}
    struct stat st;int r=fstat(fd,&st);
    if(r<0||!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>ST_DOCUMENT_LIMIT){close(fd);return -1;}
    char *p=malloc((size_t)st.st_size+1);if(!p){close(fd);return -1;}
    r=st_read_all(fd,p,(size_t)st.st_size);char extra;
    if(!r&&read(fd,&extra,1)!=0)r=-1;
    if(close(fd)<0)r=-1;
    if(r){free(p);return -1;}
    p[st.st_size]=0;*text=p;*length=(int)st.st_size;*exists=1;return 0;
}
typedef struct {
    uint32_t magic,version,bytes,base_bytes,caret,anchor,top,left,exists,checksum;
    uint64_t generation;
    char path[128];
} StDraft;
static int st_draft_path(const char *path,int slot,char out[256],int create)
{
    char dir[256];snprintf(dir,sizeof dir,"%s",path);char *slash=strrchr(dir,'/');
    if(slash)slash[1]=0;else strcpy(dir,"./");
    size_t n=strlen(dir);if(n+8>=sizeof dir)return -1;strcpy(dir+n,".studio");
    if(create&&mkdir(dir,0700)<0&&errno!=EEXIST)return -1;
    uint32_t id=st_hash(path,strlen(path));
    int len=snprintf(out,256,"%s/%08x.%d",dir,id,slot);return len>0&&len<256?0:-1;
}
static uint32_t st_draft_checksum(StDraft header,const char *base,const char *text)
{
    header.checksum=0;uint32_t h=st_hash(&header,sizeof header);
    for(unsigned i=0;i<header.base_bytes;i++)h=(h^(unsigned char)base[i])*16777619u;
    for(unsigned i=0;i<header.bytes;i++)h=(h^(unsigned char)text[i])*16777619u;return h;
}
static int st_read_draft(const char *path,StDraft *h,char **base,char **text)
{
    *base=*text=NULL;int fd=open(path,O_RDONLY);if(fd<0)return -1;
    int r=st_read_all(fd,h,sizeof *h);
    if(r||h->magic!=0x53544452u||h->version!=1||h->bytes>ST_DOCUMENT_LIMIT||h->base_bytes>ST_DOCUMENT_LIMIT||
       !memchr(h->path,0,sizeof h->path)||h->caret>h->bytes||h->anchor>h->bytes||h->exists>1){close(fd);return -1;}
    *base=malloc((size_t)h->base_bytes+1);*text=malloc((size_t)h->bytes+1);
    if(!*base||!*text)r=-1;
    if(!r)r=st_read_all(fd,*base,h->base_bytes);
    if(!r)r=st_read_all(fd,*text,h->bytes);
    char extra;if(!r&&read(fd,&extra,1)!=0)r=-1;if(close(fd)<0)r=-1;
    if(!r){(*base)[h->base_bytes]=0;(*text)[h->bytes]=0;
        if(h->checksum!=st_draft_checksum(*h,*base,*text)||!st_utf8(*base,(int)h->base_bytes)||!st_utf8(*text,(int)h->bytes)||
           !st_boundary(*text,(int)h->bytes,(int)h->caret)||!st_boundary(*text,(int)h->bytes,(int)h->anchor))r=-1;}
    if(r){free(*base);free(*text);*base=*text=NULL;}return r;
}
int st_forget_drafts(const char *path)
{
    char slots[2][256];int present[2]={0};
    /* Validate ownership of BOTH slots before deleting either. Slot names are
     * hashes; an unrelated recovery draft must never be deleted on collision. */
    for(int i=0;i<2;i++){
        if(st_draft_path(path,i,slots[i],0)<0)return -1;
        struct stat st;if(stat(slots[i],&st)<0){if(errno==ENOENT)continue;return -1;}
        StDraft h;char *base,*text;
        if(st_read_draft(slots[i],&h,&base,&text)<0)return -1;
        free(base);free(text);if(strcmp(h.path,path)){errno=EEXIST;return -1;}present[i]=1;
    }
    for(int i=0;i<2;i++)if(present[i]&&unlink(slots[i])<0)return -1;
    /* The tree hides .studio. Leave no empty metadata directory that would
     * make an apparently empty project folder impossible to delete. Other
     * documents' drafts keep it nonempty and rmdir leaves them untouched. */
    char *slash=strrchr(slots[0],'/');if(slash){*slash=0;rmdir(slots[0]);}
    return 0;
}
int st_checkpoint(StDocument *d)
{
    if(d->checkpoint==d->revision)return 0;
    uint64_t generation=d->draft_generation+1;if(!generation)return -1;
    char path[256];if(st_draft_path(d->path,(int)(generation&1),path,1)<0)return -1;
    /* A hash names the slot but never identifies its owner. A collision with
     * another valid draft is a refusal, not permission to replace that draft. */
    StDraft old;char *ob,*ot;
    if(st_read_draft(path,&old,&ob,&ot)==0){free(ob);free(ot);if(strcmp(old.path,d->path))return -1;}
    StDraft h={0};h.magic=0x53544452u;h.version=1;h.bytes=(unsigned)d->length;h.base_bytes=(unsigned)d->base_length;
    h.caret=(unsigned)d->caret;h.anchor=(unsigned)d->anchor;h.top=(unsigned)d->top;h.left=(unsigned)d->left;
    h.exists=(unsigned)d->exists;h.generation=generation;strcpy(h.path,d->path);h.checksum=st_draft_checksum(h,d->base,d->text);
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return -1;
    int r=st_write_all(fd,&h,sizeof h);
    if(!r)r=st_write_all(fd,d->base,(size_t)d->base_length);
    if(!r)r=st_write_all(fd,d->text,(size_t)d->length);
    if(!r&&fsync(fd)<0)r=-1;if(close(fd)<0)r=-1;
    if(!r){d->checkpoint=d->revision;d->draft_generation=generation;}return r;
}
int st_open_document(StDocument *d,const char *path)
{
    char *text;int bytes,exists;if(st_read_file(path,&text,&bytes,&exists)<0)return -1;
    int r=st_init(d,path,text,bytes,exists);free(text);if(r<0)return -1;
    StDraft best={0};char *bb=NULL,*bt=NULL;
    for(int i=0;i<2;i++){char slot[256],*b,*t;StDraft h;
        if(st_draft_path(path,i,slot,0)<0||st_read_draft(slot,&h,&b,&t)<0)continue;
        if(strcmp(h.path,path)||h.generation<=best.generation){free(b);free(t);continue;}
        free(bb);free(bt);best=h;bb=b;bt=t;}
    if(bt){
        /* A clean checkpoint is only a cursor bookmark. Restoring its bytes
         * after another editor saves would silently hide that external edit. */
        if(best.exists&&best.bytes==best.base_bytes&&!memcmp(bt,bb,best.bytes)){
            if(!exists){free(bb);free(bt);st_dispose(d);return -2;}
            if(d->length==(int)best.bytes&&!memcmp(d->text,bt,best.bytes)){d->caret=(int)best.caret;d->anchor=(int)best.anchor;}
            d->draft_generation=best.generation;free(bb);free(bt);return 0;
        }
        if(st_reserve(d,(int)best.bytes)<0){free(bb);free(bt);st_dispose(d);return -1;}
        int matches_disk=exists&&d->base_length==(int)best.bytes&&!memcmp(d->base,bt,best.bytes);
        memcpy(d->text,bt,(size_t)best.bytes+1);d->length=(int)best.bytes;
        if(!matches_disk){free(d->base);d->base=bb;bb=NULL;d->base_length=(int)best.base_bytes;d->exists=(int)best.exists;}
        d->caret=(int)best.caret;d->anchor=(int)best.anchor;d->top=best.top<(unsigned)d->length?(int)best.top:0;
        d->left=best.left<ST_DOCUMENT_LIMIT?(int)best.left:0;d->revision=best.generation+1;
        d->draft_generation=best.generation;
        free(bb);free(bt);return st_dirty(d)?1:0;
    }
    /* ENOENT is useful to the save conflict checker, but opening a missing
     * session path must not manufacture an empty document. Only a valid dirty
     * recovery draft above may keep a deleted file available to the user. */
    if(!exists){st_dispose(d);return -2;}
    return 0;
}
int st_save_document(StDocument *d)
{
    char *current;int bytes,exists;
    if(st_read_file(d->path,&current,&bytes,&exists)<0)return -1;
    int conflict=exists!=d->exists||bytes!=d->base_length||(bytes>0&&memcmp(current,d->base,(size_t)bytes));
    free(current);if(conflict)return -2;
    char *new_base=malloc((size_t)d->length+1);if(!new_base)return -1;
    memcpy(new_base,d->text,(size_t)d->length+1);
    if(st_checkpoint(d)<0){free(new_base);return -1;}
    int fd=open(d->path,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0){free(new_base);return -1;}
    int r=st_write_all(fd,d->text,(size_t)d->length);
    if(!r&&fsync(fd)<0)r=-1;if(close(fd)<0)r=-1;
    if(r){free(new_base);return -1;}
    free(d->base);d->base=new_base;d->base_length=d->length;d->exists=1;
    d->revision++;d->checkpoint=0;
    /* If the second checkpoint fails, disk already contains the saved draft.
     * Open recognizes that byte identity, keeping it clean on recovery. */
    st_checkpoint(d);return 0;
}
