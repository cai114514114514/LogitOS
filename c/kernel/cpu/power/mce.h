#ifndef LOGIT_MCE_H
#define LOGIT_MCE_H
void mce_init(void);
void mce_handle(void);
void mce_entry(void); /* raw #MC interrupt entry; no generic IRQ dispatch */
#endif
