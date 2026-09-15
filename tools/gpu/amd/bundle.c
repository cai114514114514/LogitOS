/* Offline packaging tool: never maps a GPU or executes an input blob. The
 * explicit MC address is a placement proposal, not an allocation or load. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amd/polaris/firmware/bundle.h"
static void *read_file(const char *dir,const char *name,size_t *size)
{
    char path[4096];
    if (snprintf(path,sizeof path,"%s/polaris10_%s.bin",dir,name)>=(int)sizeof path) return NULL;
    FILE *f=fopen(path,"rb"); if (!f) return NULL;
    if (fseek(f,0,SEEK_END)) { fclose(f);return NULL; }
    long n=ftell(f);
    if (n<=0 || n>4*1024*1024 || fseek(f,0,SEEK_SET)) { fclose(f);return NULL; }
    void *p=malloc((size_t)n);
    if (!p) { fclose(f);return NULL; }
    size_t got=fread(p,1,(size_t)n,f);int err=ferror(f);fclose(f);
    if (got!=(size_t)n || err) { free(p);return NULL; }
    *size=(size_t)n;return p;
}
int main(int argc,char **argv)
{
    if (argc!=4) { fprintf(stderr,"usage: %s firmware-directory output.bin proposed_mc_base\n",argv[0]);return 2; }
    char *end;errno=0;uint64_t base=strtoull(argv[3],&end,0);
    if (errno || end==argv[3] || *end || argv[3][0]=='-') return 2;
    static const char *names[]={"sdma","sdma1","ce","pfp","me","mec","rlc"};
    struct polaris_fw_sources sources={0};struct polaris_fw_bundle_info info;
    struct polaris_fw_view smc;size_t smc_bytes=0,mec2_bytes=0;
    void *smc_data=NULL,*mec2=NULL;uint8_t *dst=NULL;int rc=1;
    for (unsigned i=0;i<7;++i) {
        sources.file[i].data=read_file(argv[1],names[i],&sources.file[i].bytes);
        if (!sources.file[i].data) { fprintf(stderr,"missing input %s\n",names[i]);goto done; }
    }
    smc_data=read_file(argv[1],"smc",&smc_bytes);
    if (polaris_fw_parse(POLARIS_FW_SMC,smc_data,smc_bytes,&smc)) goto done;
    /* Linux VI deliberately uses MEC1 for both JTs. mec2 is not a dependency;
     * when supplied, verify its format and identity so a mismatched directory
     * is reported instead of silently selecting an unrelated firmware file. */
    mec2=read_file(argv[1],"mec2",&mec2_bytes);
    if (mec2 && (mec2_bytes!=sources.file[POLARIS_FW_MEC].bytes ||
                memcmp(mec2,sources.file[POLARIS_FW_MEC].data,mec2_bytes))) {
        fprintf(stderr,"MEC2 differs from Polaris MEC1\n");goto done;
    }
    if (polaris_fw_bundle_plan(&sources,base,&info)) goto done;
    dst=malloc(info.bytes_used);if (!dst) goto done;
    if (polaris_fw_bundle_stage(dst,info.bytes_used,base,&sources,&info)) goto done;
    FILE *f=fopen(argv[2],"wb");if (!f) goto done;
    size_t n=fwrite(dst,1,info.bytes_used,f);int close_rc=fclose(f);
    if (n!=info.bytes_used || close_rc) goto done;
    printf("BUNDLE bytes=%zu base=0x%llx entries=%u present=0x%x required=0x%x missing=0x%x smc_bytes=%u smc_start=0x%x mapped=0 loaded=0\n",
        info.bytes_used,(unsigned long long)base,info.inventory.entry_count,info.inventory.present_mask,
        POLARIS_SMU_TOC_REQUIRED_MASK,info.inventory.missing_mask,smc.bytes,smc.ucode_start_addr);
    for (unsigned i=0;i<9;++i) printf("IMAGE id=%u offset=%zu bytes=%u version=%u flags=%u\n",
        info.image[i].id,info.image_offset[i],info.image[i].bytes,info.image[i].version,info.image[i].flags);
    rc=0;
done:
    for (unsigned i=0;i<7;++i) free((void *)sources.file[i].data);
    free(smc_data);free(mec2);free(dst);
    if (rc) fprintf(stderr,"BUNDLE rejected input or output error\n");
    return rc;
}
