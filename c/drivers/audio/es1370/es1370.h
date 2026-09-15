#ifndef LOGIT_ES1370_H
#define LOGIT_ES1370_H

struct device;

/* Exact AudioPCI ES1370 1274:5000. ES1371/CT5880 have different codec and
 * sample-rate hardware and must never enter this driver. DRIVER_DECLARE
 * registers the matching production driver; these entries also permit host
 * lifecycle tests to call the actual implementation. */
int es1370_probe(struct device *device);
void es1370_remove(struct device *device);

#endif
