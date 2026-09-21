#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exit codes shared by agent_gateway.py and agent_session.py.

One jar, two doors: the gateway RAISES this code and the session DECIDES on
it. Spelling the number in both files would be two places to get it wrong,
and the way it would fail is the bad way -- the session would stop
recognising "no credentials", and a keyless checkout would go back to a hard
failure instead of an offline desktop, with nothing saying why.
"""

# The env file names no usable provider key. Not an error: the session boots
# offline and says so. Distinct from 1, which stays "the gateway broke", so a
# real crash still fails the run rather than quietly costing you the model.
NO_CREDENTIALS = 3
