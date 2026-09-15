/* Box-tree flattening uses the real parser/cascade/layout pipeline. DOM
 * parent pointers must stay intact for selector, inheritance and events. */
#define main intrinsic_original_main
#include "intrinsic_test.c"
#undef main
static int xpos(const char *id){int x,y,w,h;CHECK(layout_node_box(ID(id),&x,&y,&w,&h),"box exists");return x;}
static int ypos(const char *id){int x,y,w,h;CHECK(layout_node_box(ID(id),&x,&y,&w,&h),"box exists");return y;}
static int boxless(const char *id){return !layout_node_box(ID(id),0,0,0,0);}
int main(void){
 css_init();css_viewport(400,600);
 page("<style>body{margin:0}.contents{display:contents}#row{display:flex;width:300px}.item{width:30px;height:20px;flex:none}.contents>.item{color:red}</style><div id='row'><div id='wrap' class='contents' style='padding:100px;border:9px solid;background:blue'><div id='a' class='item'>A</div><div class='contents'><div id='b' class='item'>B</div></div></div><div id='c' class='item'>C</div></div>",400);
 EQ(xpos("a"),0,"contents first child is a flex item");EQ(xpos("b"),30,"nested contents children join flex row");EQ(xpos("c"),60,"following sibling follows flattened descendants");
 EQ(ypos("b"),ypos("a"),"contents children do not form a vertical stack");
 CHECK(boxless("wrap"),"contents wrapper has no principal box");
 CHECK(ID("a")->parent==ID("wrap"),"DOM parent survives box-tree flattening");
 EQ(((struct cstyle*)ID("a")->style)->color,0xff0000,"DOM child selector survives box-tree flattening");
 page("<style>body{margin:0}#grid{display:grid;grid-template-columns:30px 50px}.contents{display:contents}</style><div id='grid'><div id='wrap' class='contents'><div id='a'>A</div><div id='b'>B</div></div></div>",400);
 EQ(width("a"),30,"flattened first grid item uses first track");EQ(xpos("b"),30,"flattened second grid item uses next track");EQ(width("b"),50,"flattened second grid item uses second track");
 page("<style>body{margin:0}.contents{display:contents}#owner{height:200px}</style><div id='owner'><div id='wrap' class='contents' style='height:1px'><div id='a' style='height:50%'></div></div></div>",400);
 EQ(height("a"),100,"percentage height skips boxless ancestor");CHECK(boxless("wrap"),"contents height never creates a box");
 page("<style>body{margin:0}.contents{display:contents}#row{display:flex}.item{width:30px;flex:none}#wrap::before{content:'X';display:block;width:20px}#wrap::after{content:'Y';display:block;width:10px}</style><div id='row'><div id='wrap' class='contents'><div id='a' class='item'>A</div></div><div id='b' class='item'>B</div></div>",400);
 EQ(xpos("a"),20,"contents before pseudo participates in flattened sequence");EQ(xpos("b"),60,"contents pseudo does not lose following sibling");
 page("<style>body{margin:0}.contents{display:contents}#wrap{display:none}</style><div id='wrap' class='contents'><div id='a'>HIDDEN</div></div>",400);
 CHECK(boxless("a"),"cascade display none still prunes subtree");
 page("<style>body{margin:0}#row{display:flex}.contents{display:contents}.inherit{display:inherit}.item{width:30px;flex:none}</style><div id='row'><div class='contents'><div class='inherit'><div class='inherit'><div class='item' id='a'>A</div><div class='item' id='b'>B</div></div></div></div></div>",400);
 EQ(xpos("b"),30,"inherited contents chains flatten through all wrappers");
 page("<style>body{margin:0}#row{display:flex}.contents{display:contents}.item{width:30px;flex:none}</style><div id='row'><button class='contents' id='wrap'><div class='item' id='a'>A</div></button><div class='item' id='b'>B</div></div>",400);
 EQ(xpos("b"),30,"button contents keeps authored children");CHECK(boxless("wrap"),"button contents has no control box");
 page("<style>body{margin:0}.contents{display:contents}</style><select id='wrap' class='contents'><option id='a'>HIDDEN</option></select><img id='photo' class='contents' width=30 height=30>",400);
 CHECK(boxless("wrap")&&boxless("a")&&boxless("photo"),"contents replaced and select implementation subtree do not render");
 printf("display-contents: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
