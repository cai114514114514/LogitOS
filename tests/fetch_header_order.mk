# Derive the complete transport list from the existing real-adapter gate;
# only replace its main (its socket/server fixture is textually reused).
FETCH_HEADER_SRC := tests/unit/fetch_header_order_test.c $(filter-out tests/unit/h2mux_test.c,$(H2MUX_SRC))


