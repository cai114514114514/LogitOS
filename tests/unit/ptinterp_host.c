/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "elf.h"
#include "space.h"
static unsigned char *loader;static size_t loader_size;static int deny,opened,closed,checks,failed;
#define CHECK(c,n) do{checks++;if(!(c)){failed++;fprintf(stderr,"FAIL %s\n",n);}}while(0)
/* Filesystem/device doubles only. The production interp.c owns permission,
 * size and cache-handle decisions; production elf.c reads its streaming path. */
int vfs_access(const char *p,int want){CHECK(want==5,"read and execute requested");return deny||strcmp(p,"/lib/ld-logit-test.so")?-1:0;}
int vfs_size(const char *p){(void)p;return (int)loader_size;}
int pcache_file_open(const char *p){(void)p;opened++;return 17;}
void pcache_file_put(int h){CHECK(h==17,"own interpreter handle");closed++;}
int vfs_pread(const char *p,void *buf,int n,long long off){(void)p;if(off<0||(uint64_t)off>loader_size||(uint64_t)n>loader_size-off)return -1;memcpy(buf,loader+off,n);return n;}
static uint64_t u64(const void*p){uint64_t n;memcpy(&n,p,8);return n;}
static uint32_t u32(const void*p){uint32_t n;memcpy(&n,p,4);return n;}
static uint16_t u16(const void*p){uint16_t n;memcpy(&n,p,2);return n;}
static struct elf64_phdr *ph(unsigned char*b,unsigned t){struct elf64_phdr*p=(void*)(b+u64(b+32));for(int i=0;i<u16(b+56);i++)if(p[i].p_type==t)return p+i;return 0;}
static uint64_t sym(unsigned char*b,const char*name){unsigned char*s=b+u64(b+40);for(int i=0;i<u16(b+60);i++,s+=64)if(u32(s+4)==2){unsigned char*sh=b+u64(b+40);char*str=(void*)(b+u64(sh+u32(s+40)*64+24));unsigned char*t=b+u64(s+24);for(uint64_t j=0;j<u64(s+32);j+=24)if(!strcmp(str+u32(t+j),name))return u64(t+j+8);}abort();}
static unsigned char *file(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f)exit(2);fseek(f,0,SEEK_END);*n=ftell(f);rewind(f);unsigned char*b=malloc(*n);if(fread(b,1,*n,f)!=*n)exit(2);fclose(f);return b;}
int main(int argc,char**argv)
{
    if(argc!=3)return 2;size_t n;unsigned char*b=file(argv[1],&n),*m=malloc(n);loader=file(argv[2],&loader_size);
    space_set_nx(1);space_quiet(1);struct elf_image im;uint64_t pages=0;
    for(int pass=0;pass<2;pass++) {
        space_reset();int rc=elf_load_image(b,n,&im);CHECK(!rc,"interpreter pair loads");if(rc){fprintf(stderr,"%s\n",space_last_msg());return 1;}
        CHECK(im.entry==im.load_bias+u64(b+24),"main entry retained");
        CHECK(im.start_entry==im.interp_base+u64(loader+24)&&im.start_entry!=im.entry,"START_ASSERT");
        CHECK(im.interp_base>im.load_bias&&im.interp_base>=(1ull<<40),"BASE_ASSERT");
        CHECK(im.tls_tp==0&&im.relocations==0,"DEFER_ASSERT");
        CHECK(*(uint64_t*)(im.load_bias+sym(b,"pie_pointer"))!=im.load_bias+sym(b,"pie_data"),"UNRELOCATED_ASSERT");
        CHECK(space_writable(im.load_bias+sym(b,"pie_relro")),"RELRO_ASSERT");
        CHECK(!space_writable(im.start_entry)&&!space_nx(im.start_entry),"interpreter text RX");
        CHECK(space_nx(im.interp_base+sym(loader,"interp_pointer")),"interpreter data NX");
        CHECK(im.phdr_va==im.load_bias+ph(b,6)->p_vaddr,"main real PHDR");
        CHECK(opened==closed,"interpreter transient reference balanced");
        pages=space_frames_used();
    }
    if(failed) { space_reset();free(b);free(m);free(loader);printf("PTINTERP_HOST checks=%d failures=%d\n",checks,failed);return 1; }
    /* A fixed main deliberately occupies the NEXT deterministic interpreter
     * slot. The two objects must not overwrite one another, even when that
     * interpreter base was chosen from a separate nominal window. */
    uint64_t itop=0;struct elf64_phdr *iph=(void*)(loader+u64(loader+32));
    for(int j=0;j<u16(loader+56);j++)if(iph[j].p_type==1&&iph[j].p_vaddr+iph[j].p_memsz>itop)itop=iph[j].p_vaddr+iph[j].p_memsz;
    uint64_t actual=im.top-((itop+4095)&~4095ull)-4096;
    uint64_t next=(actual&~0xffffffffull)|((actual+(1ull<<28))&0xffffffffull);
    memcpy(m,b,n);m[16]=2;uint64_t entry=next+u64(m+24);memcpy(m+24,&entry,8);
    struct elf64_phdr *table=(void*)(m+u64(m+32));
    for(int j=0;j<u16(m+56);j++)if(table[j].p_type==1||table[j].p_type==2||table[j].p_type==6||table[j].p_type==7||table[j].p_type==0x6474e552)table[j].p_vaddr+=next;
    space_reset();CHECK(elf_load_image(m,n,&im)==ELF_E_INTERP,"address collision refused");CHECK(strstr(space_last_msg(),"overlaps")!=0,"collision diagnosed before interpreter mapping");CHECK(*(long*)(next+sym(b,"pie_data"))==42,"collision preserves main data");CHECK(opened==closed,"collision closes reference");
    memcpy(m,b,n);ph(m,6)->p_type=4;space_reset();CHECK(!elf_load_image(m,n,&im),"PT_PHDR optional");CHECK(im.phdr_va==im.load_bias+u64(m+32),"PHDR inferred from LOAD");
    memcpy(m,b,n);struct elf64_phdr*i=ph(m,3);m[i->p_offset]='x';space_reset();CHECK(elf_load_image(m,n,&im)==ELF_E_INTERP,"relative path refused");CHECK(!space_frames_used(),"invalid path maps nothing");
    memcpy(m,b,n);i=ph(m,3);m[i->p_offset+i->p_filesz-1]='x';space_reset();CHECK(elf_load_image(m,n,&im)==ELF_E_INTERP,"unterminated path refused");
    memcpy(m,b,n);i=ph(m,3);i->p_filesz=129;space_reset();CHECK(elf_load_image(m,n,&im)==ELF_E_INTERP,"long path refused");
    memcpy(m,b,n);i=ph(m,3);m[i->p_offset+2]=0;space_reset();CHECK(elf_load_image(m,n,&im)==ELF_E_INTERP,"embedded NUL refused");
    memcpy(m,b,n);*ph(m,7)=*ph(m,3);space_reset();CHECK(elf_load_image(m,n,&im)==ELF_E_INTERP,"duplicate interpreter refused");
    deny=1;space_reset();CHECK(elf_load_image(b,n,&im)==ELF_E_INTERP,"permission refusal");deny=0;CHECK(opened==closed,"permission refusal does not open");
    unsigned char*saved=loader;size_t saved_n=loader_size;loader=b;loader_size=n;space_reset();CHECK(elf_load_image(b,n,&im)==ELF_E_INTERP,"nested interpreter refused");loader=saved;loader_size=saved_n;CHECK(opened==closed,"nested refusal closes handle");
    /* Failure in either object's physical allocations is undone by the
     * caller dropping the private address space, just as execve does. */
    int late_oom=0;
    for(uint64_t budget=0;budget<=pages;budget++){space_reset();space_set_budget(budget);int before=opened;int rc=elf_load_image(b,n,&im);if(rc==ELF_E_OOM&&opened>before)late_oom++;CHECK(rc==0||rc==ELF_E_OOM,"allocation result");CHECK(opened==closed,"allocation failure closes handle");space_reset();CHECK(!space_frames_used(),"allocation rollback");}
    CHECK(late_oom>0,"interpreter allocation failure actually reached");
    free(b);free(m);free(loader);printf("PTINTERP_HOST checks=%d failures=%d opens=%d closes=%d\n",checks,failed,opened,closed);return failed?1:0;
}
