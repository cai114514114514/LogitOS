/* The same parser -> cascade -> layout -> GUI pixel recorder as the original
 * background gate. CSV is shared with guest scanout; no duplicated colors. */
#define CSS_BACKGROUND_WIDTH 640
#define CSS_BACKGROUND_HEIGHT 520
#define CSS_BACKGROUND_CHECKS 49
#define CSS_BACKGROUND_HTML "tests/fixtures/engine-expansion/css-gradients.html"
#define CSS_BACKGROUND_CSV "tests/fixtures/engine-expansion/css-gradients-pixels.csv"
#define CSS_BACKGROUND_NAME "css-gradient-pixels"
#include "css_background_test.c"
