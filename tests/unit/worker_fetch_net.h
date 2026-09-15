#ifndef LOGIT_WORKER_FETCH_TEST_NET_H
#define LOGIT_WORKER_FETCH_TEST_NET_H
void wft_net_reset(void);
int wft_net_live(void);
int wft_net_requests(const char *host, const char *target, const char *method);
int wft_net_header(const char *host, const char *target, const char *header);
void wft_net_release(const char *target, const char *body);
void wft_net_finish_all(void);
#endif
