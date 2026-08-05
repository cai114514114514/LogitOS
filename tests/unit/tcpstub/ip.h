#ifndef LOGIT_IP_H
#define LOGIT_IP_H
#include <stdint.h>
#define IP_PROTO_TCP 6
int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len);
#endif
