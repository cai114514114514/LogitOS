/* Reach the actual callback registered by browser_load. No new production
 * testing API: this transport invokes the same callback as bfetch_wait while
 * its deterministic clock advances. It never dispatches JS itself. */
#include "loader_fakebfetch.c"
void progress_paint_tick(void) { if (g_tick) g_tick(); }
