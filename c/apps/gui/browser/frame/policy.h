#ifndef LOGIT_IFRAME_POLICY_H
#define LOGIT_IFRAME_POLICY_H

enum { IF_POLICY_STYLE = 1, IF_POLICY_IMAGE = 2, IF_POLICY_CONNECT = 3 };
/* Enforced header policies only, joined with newlines; every policy intersects.
 * The loader must reject missing/truncated metadata before calling this API.
 * The current embedded-document owner has one parent (no nested frames). */
int iframe_policy_frame(const char *parent_url, const char *child_url,
                        const char *parent_csp, const char *child_csp,
                        const char *child_xfo);
int iframe_policy_resource(const char *document_url, const char *resource_url,
                           const char *document_csp, int kind);
int iframe_policy_base(const char *document_url, const char *base_url,
                       const char *document_csp);
int iframe_policy_inline_style(const char *document_csp, const char *nonce);
int iframe_policy_style_attribute(const char *document_csp);
int iframe_policy_inline_script(const char *document_csp, const char *nonce);
int iframe_policy_script_attribute(const char *document_csp);
int iframe_policy_eval(const char *document_csp);
/* Blob entry workers and their classic imports. Blob capabilities themselves
 * are additionally checked in the creator's native object-URL registry. */
int iframe_policy_blob_worker(const char *csp);
/* Classic network worker entry: same-origin, mixed-content refusal and the
 * worker-src fallback chain. Recheck each redirect before requesting it. */
int iframe_policy_network_worker(const char *document_url,const char *url,const char *csp);
int iframe_policy_worker_import(const char *document_url,const char *url,const char *csp);
/* Classic external scripts only. parser_inserted comes from native loader
 * provenance, never an author-writable attribute. Hash-only policies refuse
 * until integrity metadata/digest checking is implemented. */
int iframe_policy_script(const char *document_url, const char *resource_url,
                         const char *csp, const char *nonce, int parser_inserted);
#endif
