/* SPDX-License-Identifier: MIT */
/* Filesystem identities, rather than guessed inode numbers or hashes, bind a
 * task to its Project and documents. v4 tasks keep their explicit legacy path
 * behavior; a v5 task never silently falls back to a reused pathname. */
static int bound_info(const char *p,struct logit_file_id *id,uint64_t *revision)
{
    void *data=0;size_t n=0;int rc=ag_userfs(service_uid,service_gid,AG_U_REF,p,0,0,&data,&n,0);
    if(!rc&&n==sizeof(struct ag_file_info)){struct ag_file_info *info=data;*id=info->id;if(revision)*revision=info->revision;}else if(!rc)rc=AG_E_IO;
    free(data);return rc;
}
static int bound_ref(const char *p,struct logit_file_id *id)
{return bound_info(p,id,0);}
static int bound_path(const struct logit_file_id *id,const char *hint,char *out)
{
    struct logit_file_id current;
    if(hint&&hint[0]&&!ag_file_ref(hint,&current,0)&&ag_file_same(id,&current))return copy(out,AG_PATH,hint);
    void *data=0;size_t n=0;
    int rc=ag_userfs(service_uid,service_gid,AG_U_RESOLVE,"/",id,sizeof *id,&data,&n,0);
    if(!rc&&(n<2||n>AG_PATH||((char *)data)[n-1]))rc=AG_E_IO;
    if(!rc)rc=copy(out,AG_PATH,data);free(data);return rc;
}
static int bound_attach(struct slot *s,const char *project,const char *origin)
{
    int rc=bound_ref(project,&s->binding.refs[AG_BIND_PROJECT]);
    if(rc==AG_E_VERSION)return 0; /* old volume, no persistent identity claim */
    if(rc<0)return rc;
    if(origin&&origin[0]){
        rc=bound_info(origin,&s->binding.refs[AG_BIND_ORIGIN],&s->binding.revisions[AG_BIND_ORIGIN]);
        if(rc!=AG_E_NOTFOUND&&rc<0)return rc; /* unsaved new document has no FileID yet */
    }
    return ag_binding_save(s->dir,s->task.id,&s->binding);
}
static int bound_refresh(struct slot *s)
{
    if(!s->binding.journal.id)return 0;
    char hint[AG_PATH],project[AG_PATH],output[AG_PATH],artifact[AG_PATH];
    ag_parent_path(s->task.output,hint,sizeof hint);
    int rc=bound_path(&s->binding.refs[AG_BIND_PROJECT],hint,project);if(rc<0)return rc;
    const char *leaf=strrchr(s->task.output,'/');if(!leaf)return AG_E_ARGUMENT;
    rc=path(output,project,leaf+1);if(rc<0)return rc;
    if(s->binding.refs[AG_BIND_OUTPUT].object){
        rc=bound_path(&s->binding.refs[AG_BIND_OUTPUT],output,output);if(rc<0)return rc;
        char parent[AG_PATH];struct logit_file_id id;
        ag_parent_path(output,parent,sizeof parent);rc=bound_ref(parent,&id);
        if(rc<0||!ag_file_same(&id,&s->binding.refs[AG_BIND_PROJECT]))return AG_E_SCOPE;
    }
    artifact[0]=0;
    if(s->task.artifact[0]){
        leaf=strrchr(s->task.artifact,'/');if(!leaf)return AG_E_ARGUMENT;
        rc=path(artifact,project,leaf+1);if(rc<0)return rc;
        if(!s->task.pending_operation&&s->binding.refs[AG_BIND_ARTIFACT].object){
            rc=bound_path(&s->binding.refs[AG_BIND_ARTIFACT],artifact,artifact);if(rc<0)return rc;
        }
    }
    if(!s->task.pending_operation&&s->binding.refs[AG_BIND_ARTIFACT].object){
        char parent[AG_PATH];struct logit_file_id id;uint64_t revision;
        ag_parent_path(artifact,parent,sizeof parent);rc=bound_ref(parent,&id);
        if(rc<0||!ag_file_same(&id,&s->binding.refs[AG_BIND_PROJECT]))return AG_E_SCOPE;
        rc=bound_info(artifact,&id,&revision);if(rc<0)return rc;
        if(revision!=s->binding.revisions[AG_BIND_ARTIFACT])return AG_E_CONFLICT;
    }
    if(strcmp(output,s->task.output)||strcmp(artifact,s->task.artifact)){
        copy(s->task.output,sizeof s->task.output,output);copy(s->task.artifact,sizeof s->task.artifact,artifact);
        return persist(s);
    }
    return 0;
}
static int bound_published(struct slot *s)
{
    if(!s->binding.journal.id)return 0;
    int rc=bound_info(s->task.artifact,&s->binding.refs[AG_BIND_ARTIFACT],&s->binding.revisions[AG_BIND_ARTIFACT]);if(rc<0)return rc;
    if(!s->binding.refs[AG_BIND_OUTPUT].object){s->binding.refs[AG_BIND_OUTPUT]=s->binding.refs[AG_BIND_ARTIFACT];s->binding.revisions[AG_BIND_OUTPUT]=s->binding.revisions[AG_BIND_ARTIFACT];}
    return ag_binding_save(s->dir,s->task.id,&s->binding);
}
static int bound_matches(struct slot *s,const char *p,const struct logit_file_id *id)
{
    if(s->binding.journal.id){
        for(unsigned i=AG_BIND_ORIGIN;i<AG_BIND_COUNT;i++)if(ag_file_same(id,&s->binding.refs[i])){
            struct logit_file_id current;uint64_t revision;
            return !bound_info(p,&current,&revision)&&revision==s->binding.revisions[i];
        }
        return 0;
    }
    return !strcmp(s->task.output,p)||!strcmp(s->task.artifact,p)||
        (s->review.enabled&&!strcmp(s->review.record.artifact,p));
}

static int bound_ready(struct slot *s)
{
    int rc=bound_refresh(s);if(rc>=0)return rc;
    if(s->task.phase==AG_CANCELLED)return rc;
    const char *reason=rc==AG_E_CONFLICT?
        "Document changed outside this task. Your disk version is preserved; review the task before resuming.":
        "Project or document is unavailable or outside its original scope. No new files will be submitted.";
    if(s->task.phase!=AG_RECONCILE||strcmp(s->task.reason,reason)){
        s->task.phase=AG_RECONCILE;copy(s->task.reason,sizeof s->task.reason,reason);persist(s);
    }
    return rc;
}
/* v4 has no earlier FileID to recover. Establish the conversion baseline only
 * after checking the last acknowledged artifact against its durable receipt. */
static int bound_migrate(struct slot *s)
{
    char project[AG_PATH];struct logit_file_id id;
    int rc=ag_parent_path(s->task.output,project,sizeof project);if(rc<0)return rc;
    rc=bound_ref(project,&id);if(rc==AG_E_VERSION)return AG_E_NOTFOUND;if(rc<0)return rc;
    if(s->task.artifact[0]&&!s->task.pending_operation){
        void *data=0;size_t n=0;rc=ag_userfs(service_uid,service_gid,AG_U_READ,s->task.artifact,0,0,&data,&n,0);
        int matches=!rc&&n==s->task.pending_bytes&&ag_checksum(data,n)==s->task.pending_checksum;
        free(data);if(!matches)return AG_E_CONFLICT;
    }
    s->binding.refs[AG_BIND_PROJECT]=id;
    if(s->review.enabled&&s->review.record.artifact[0]){
        rc=bound_info(s->review.record.artifact,&s->binding.refs[AG_BIND_ORIGIN],&s->binding.revisions[AG_BIND_ORIGIN]);
        if(rc<0&&rc!=AG_E_NOTFOUND)return rc;
    }
    if(s->task.artifact[0]&&!s->task.pending_operation){
        rc=bound_info(s->task.output,&s->binding.refs[AG_BIND_OUTPUT],&s->binding.revisions[AG_BIND_OUTPUT]);if(rc<0)return rc;
        rc=bound_info(s->task.artifact,&s->binding.refs[AG_BIND_ARTIFACT],&s->binding.revisions[AG_BIND_ARTIFACT]);if(rc<0)return rc;
    }
    return ag_binding_save(s->dir,s->task.id,&s->binding);
}
