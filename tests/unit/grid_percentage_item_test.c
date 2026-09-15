/* The navigation sample uses width:100% items in repeat(N,1fr). Reducing N
 * preserves the intrinsic-cycle bug without relying on any site's names. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main
int main(void) {
 css_init();css_viewport(400,600);
 page("<style>body{margin:0}.grid{display:grid;width:320px;grid-template-columns:repeat(3,1fr);gap:10px}.grid>a{display:inline-block;width:100%;height:26px;border:1px solid;box-sizing:content-box}</style><div class='grid'><a id='a'>A</a><a id='b'>B</a><a id='c'>C</a></div>",400);
 EQ(width("a"),102,"percent item resolves against area not entire grid");
 EQ(width("b"),102,"second fractional track avoids percent intrinsic inflation");
 page("<style>body{margin:0}.grid{display:grid;width:320px;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px}#a{width:50%;height:20px}#b{grid-column:span 2;width:calc(100% - 10px);height:20px}</style><div class='grid'><div id='a'>A</div><div id='b'>B</div></div>",400);
 EQ(width("a"),50,"half width resolves against single minmax area");
 EQ(width("b"),200,"spanning percent calc includes interior gap");
 page("<style>body{margin:0}.grid{display:grid;width:300px;grid-template-columns:repeat(3,1fr)}#a{width:100%;max-width:60px;height:20px}#b{width:50%;min-width:70px;height:20px}#c{width:100%;padding:10px;box-sizing:border-box;height:20px}</style><div class='grid'><div id='a'>A</div><div id='b'>B</div><div id='c'>C</div></div>",400);
 EQ(width("a"),60,"deferred width retains max constraint");
 EQ(width("b"),70,"deferred width retains min constraint");
 EQ(width("c"),100,"border-box percent width includes padding");
 page("<style>body{margin:0}.grid{display:grid;width:200px;grid-template-columns:1fr 1fr}#a{width:40px;height:20px}#b{height:20px}</style><div class='grid'><div id='a'>A</div><div id='b'>B</div></div>",400);
 EQ(width("a"),40,"fixed width remains definite");EQ(width("b"),100,"auto item retains stretching");
 printf("grid-percentage-item: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
