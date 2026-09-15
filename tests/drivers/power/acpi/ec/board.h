#ifndef LOGIT_TEST_EC_BOARD_H
#define LOGIT_TEST_EC_BOARD_H
#include "model.h"
extern struct ec_model power_ec_model;
void power_ec_board_init(const char *scenario);
void power_ec_board_raise_query(uint8_t query);
void power_ec_board_drain(void);
unsigned power_ec_board_gpe_enabled(void);
unsigned power_ec_board_early_gpe_enable(void);
#endif
