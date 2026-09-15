/* End-to-end grid unit bridge: the pure parser formerly had only one font
 * basis, so passing its tests could not distinguish rem from em. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main
int main(void){
 css_init();css_viewport(400,600);
 page("<style>html{font-size:20px}body{margin:0}.grid{display:grid;font-size:32px;grid-template-columns:2rem 2em 1fr;width:300px}</style><div class='grid'><div id='rem'>A</div><div id='em'>B</div><div id='rest'>C</div></div>",400);
 EQ(width("rem"),40,"rem track uses actual root font");EQ(width("em"),64,"em track uses element font");EQ(width("rest"),196,"remaining fraction follows both resolved font units");
 page("<style>html{font-size:24px}body{margin:0}#grid{display:grid;font-size:12px;grid-template-columns:1fr;grid-auto-rows:2rem 2em}</style><div id='grid'><div id='rem'>A</div><div id='em'>B</div></div>",400);
 EQ(height("rem"),48,"implicit rem row uses root font");EQ(height("em"),24,"implicit em row uses local font");
 printf("grid-font-units: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
