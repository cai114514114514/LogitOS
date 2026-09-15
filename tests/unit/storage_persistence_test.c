#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage_backend.h"
#include "tabs.h"
static int checks,fails;
static void ck(int ok,const char *s){checks++;printf("%s: %s\n",ok?"ok":"FAIL",s);if(!ok)fails++;}
static unsigned char *files[2];static int sizes[2],writes,write_mode,read_mode;
static int slot_of(const char *p){return p[strlen(p)-1]=='1';}
static int rd(const char *p,void *b,int n){int i=slot_of(p);if(read_mode || !files[i])return -1;if(n<sizes[i])return -1;n=sizes[i];memcpy(b,files[i],n);return n;}
static int wr(const char *p,const void *b,int n){writes++;int i=slot_of(p);if(write_mode==1)return -1;if(write_mode==4)return 0;free(files[i]);files[i]=malloc(n);memcpy(files[i],b,n);sizes[i]=n;if(write_mode==2){sizes[i]--;return n-1;}if(write_mode==3)files[i][n-1]^=1;return write_mode==5?0:n;}
static int md(const char *p){(void)p;return 0;}
static const struct bstore_ops fake={rd,wr,md};
static struct storage_key local={"https://store.example",STORAGE_LOCAL,0},other={"https://other.example",STORAGE_LOCAL,0},session={"https://store.example",STORAGE_SESSION,42};
static int eq(const struct storage_key *key,const char *k,size_t kn,const char *v,size_t vn){size_t n;const char *got=storage_backend_get(key,k,kn,&n);return got && n==vn && !memcmp(got,v,n);}
static void fresh(void){storage_backend_set_store(NULL);for(int i=0;i<2;i++){free(files[i]);files[i]=NULL;sizes[i]=0;}writes=write_mode=read_mode=0;ck(storage_backend_set_store(&fake)==0,"fresh store opens");}
static void seed(void){fresh();ck(storage_backend_set(&local,"x",1,"old",3)==0,"seed commits");}
static const char *directory;
static void filename(char *out,const char *path){snprintf(out,1024,"%s/%s",directory,strrchr(path,'/')+1);}
static int disk_rd(const char *p,void *b,int n){char path[1024];filename(path,p);FILE *f=fopen(path,"rb");if(!f)return -1;if(fseek(f,0,SEEK_END)){fclose(f);return -1;}long size=ftell(f);if(size<0 || size>n){fclose(f);return -1;}rewind(f);int got=(int)fread(b,1,n,f);int bad=ferror(f);fclose(f);return bad?-1:got;}
static int disk_wr(const char *p,const void *b,int n){char path[1024];filename(path,p);FILE *f=fopen(path,"wb");if(!f)return -1;size_t wrote=fwrite(b,1,n,f);int closed=fclose(f);return wrote==(size_t)n && !closed?n:-1;}
static const struct bstore_ops disk={disk_rd,disk_wr,md};
int main(int argc,char **argv)
{
 if(argc==3){directory=argv[2];ck(storage_backend_set_store(&disk)==0,"disk store loads");
  if(!strcmp(argv[1],"write")){ck(storage_backend_set(&local,"cross-process",13,"durable bytes",13)==0,"real file write commits");ck(storage_backend_set(&session,"only-tab",8,"private",7)==0,"session write stays local");}
  else{ck(eq(&local,"cross-process",13,"durable bytes",13),"new process restores exact local bytes");ck(storage_backend_length(&session)==0,"new process has no previous session");}
  return fails?1:0;
 }
 seed();ck(storage_backend_set(&local,"a\0b",3,"v\0z",3)==0,"NUL key value commits");ck(storage_backend_set(&other,"x",1,"separate",8)==0,"second origin commits");
 int before=writes;ck(storage_backend_set(&session,"x",1,"ephemeral",9)==0 && writes==before,"session mutation performs no disk IO");
 ck(storage_backend_set_store(&fake)==0,"reopen succeeds");ck(eq(&local,"x",1,"old",3),"reopen restores persisted local value");ck(eq(&local,"a\0b",3,"v\0z",3),"reopen preserves embedded NUL bytes");ck(eq(&other,"x",1,"separate",8),"reopen preserves separate origins");ck(storage_backend_length(&session)==0,"reopen discards all session areas");
 ck(storage_backend_remove(&local,"x",1)==0,"remove commits");ck(storage_backend_set_store(&fake)==0 && storage_backend_length(&local)==1,"remove survives reopen");size_t n;const char *key=storage_backend_key(&local,0,&n);ck(key && n==3 && !memcmp(key,"a\0b",3),"remaining key order survives reopen");
 ck(storage_backend_clear(&local)==0,"clear commits");ck(storage_backend_set_store(&fake)==0 && storage_backend_length(&local)==0 && storage_backend_length(&other)==1,"clear survives reopen without losing other origin");
 seed();ck(storage_backend_set(&local,"x",1,"new",3)==0,"second generation commits");files[1][sizes[1]-1]^=1;
 ck(storage_backend_set_store(&fake)==0 && eq(&local,"x",1,"old",3),"corrupt newest slot falls back to previous valid generation");ck(storage_backend_set(&local,"x",1,"fixed",5)==0,"recovery writes damaged slot");ck(storage_backend_set_store(&fake)==0 && eq(&local,"x",1,"fixed",5),"recovery persists");
 seed();write_mode=1;ck(storage_backend_set(&local,"x",1,"new",3)==STORAGE_IO,"negative write result is failure");ck(eq(&local,"x",1,"old",3),"write failure keeps old in-memory value");write_mode=0;ck(storage_backend_set(&local,"x",1,"retry",5)==STORAGE_IO,"uncertain commit blocks later writes until reopen");ck(storage_backend_set(&session,"x",1,"ok",2)==0,"disk failure does not disable session storage");ck(storage_backend_set_store(&fake)==0 && eq(&local,"x",1,"old",3),"failed write retained prior on-disk slot");
 seed();write_mode=2;ck(storage_backend_set(&local,"x",1,"new",3)==STORAGE_IO,"short positive write is failure");write_mode=0;ck(storage_backend_set_store(&fake)==0 && eq(&local,"x",1,"old",3),"torn slot recovers last valid value");
 seed();write_mode=3;ck(storage_backend_set(&local,"x",1,"new",3)==STORAGE_IO,"readback detects lying successful write");write_mode=0;ck(storage_backend_set_store(&fake)==0 && eq(&local,"x",1,"old",3),"readback corruption preserves old slot");
 seed();write_mode=4;ck(storage_backend_set(&local,"x",1,"new",3)==STORAGE_IO,"success without bytes is rejected");write_mode=0;ck(storage_backend_set_store(&fake)==0 && eq(&local,"x",1,"old",3),"silent no-write leaves old bytes");
 seed();write_mode=5;ck(storage_backend_set(&local,"x",1,"zero-return",11)==0,"documented zero-success adapter accepted after readback");write_mode=0;ck(storage_backend_set_store(&fake)==0 && eq(&local,"x",1,"zero-return",11),"zero-success adapter persisted exact bytes");
 seed();write_mode=1;ck(storage_backend_clear(&local)==STORAGE_IO && eq(&local,"x",1,"old",3),"clear failure preserves memory");write_mode=0;storage_backend_set_store(&fake);write_mode=1;ck(storage_backend_remove(&local,"x",1)==STORAGE_IO && eq(&local,"x",1,"old",3),"remove failure preserves memory");
 seed();files[0][0]^=1;ck(storage_backend_set_store(&fake)==STORAGE_CORRUPT,"only corrupt snapshot refuses restore");before=writes;ck(storage_backend_set(&local,"x",1,"lost",4)==STORAGE_CORRUPT && writes==before,"corrupt store is not overwritten with empty state");
 seed();storage_backend_fail_alloc_after(0);ck(storage_backend_set_store(&fake)==STORAGE_NOMEM,"restore allocation failure is explicit");storage_backend_fail_alloc_after(-1);ck(storage_backend_set_store(&fake)==0 && eq(&local,"x",1,"old",3),"allocation failure does not alter saved bytes");
 seed();storage_backend_fail_alloc_after(0);ck(storage_backend_set(&local,"x",1,"new",3)==STORAGE_NOMEM && eq(&local,"x",1,"old",3),"mutation allocation failure preserves value");storage_backend_fail_alloc_after(-1);
 char *full=malloc(STORAGE_BYTE_LIMIT);memset(full,'q',STORAGE_BYTE_LIMIT);ck(storage_backend_set(&local,"x",1,full,STORAGE_BYTE_LIMIT)==STORAGE_QUOTA,"quota enforced before persistence");free(full);
 ck(storage_backend_set_store(&fake)==0 && eq(&local,"x",1,"old",3),"quota failure preserves persisted old value");
 storage_backend_set_store(NULL);for(int i=0;i<2;i++)free(files[i]);
 printf("storage-persistence: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
