#ifndef LOGIT_IFRAME_POLICY_H
#define LOGIT_IFRAME_POLICY_H

enum { IF_POLICY_STYLE = 1, IF_POLICY_IMAGE = 2 };
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
#endif
