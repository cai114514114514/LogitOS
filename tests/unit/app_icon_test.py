# SPDX-License-Identifier: MIT
"""Exercise the real raster-icon loader with bounded fake filesystem/decoder IO."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
negative = '--negative' in sys.argv
headers = {
    'img.h': 'struct image { int w, h; unsigned char *rgba; }; int img_decode(const unsigned char *, int, struct image *);',
    'vfs.h': 'int vfs_size(const char *); int vfs_pread(const char *, void *, int, long long);',
    'fb.h': 'void fb_blit_rgba(int,int,int,int,const unsigned char *,int,int);',
    'kheap.h': '#include <stdlib.h>\n#define kmalloc malloc\n#define kfree free',
    'kprintf.h': 'int kprintf(const char *, ...);',
    'syscall.h': 'struct image; int kernel_img_decode(const unsigned char *, int, struct image *);',
}
test = r'''
#include <stdio.h>
#include <string.h>
#include "img.h"
#include "icons.h"
static unsigned char png[40] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,8,0,0,0,8};
static unsigned char pixels[8*8*4];
static int mode, reads, decoded, draws, failures;
int vfs_size(const char *path) { (void)path; reads++; return mode == 1 ? -1 : sizeof png; }
int vfs_pread(const char *path, void *out, int size, long long off)
{ (void)path; (void)off; memcpy(out,png,size); return size; }
int kernel_img_decode(const unsigned char *bytes, int size, struct image *out)
{ (void)bytes; (void)size; decoded++; *out=(struct image){8,8,pixels}; return 0; }
void fb_blit_rgba(int x,int y,int w,int h,const unsigned char *p,int sw,int sh)
{ if (x!=10 || y!=20 || w!=40 || h!=40 || p!=pixels || sw!=8 || sh!=8) failures++; draws++; }
int kprintf(const char *format, ...) { (void)format; return 0; }
#define CHECK(expr) do {if (!(expr)) {printf("FAIL %s\n",#expr);failures++;}}while(0)
int main(void) {
    CHECK(app_icon_draw("/settings.aex",10,20,40)==1);
    CHECK(draws==1 && decoded==1);
    CHECK(app_icon_draw("settings.aex",10,20,40)==1);
    CHECK(draws==2 && decoded==1 && reads==1);
    mode=1;
    CHECK(app_icon_draw("missing.aex",10,20,40)==0);
    CHECK(app_icon_draw("missing.aex",10,20,40)==0 && reads==2);
    mode=0; png[16]=127;
    CHECK(app_icon_draw("huge.aex",10,20,40)==0 && decoded==1);
    png[16]=0; png[0]=0;
    CHECK(app_icon_draw("bad.aex",10,20,40)==0 && decoded==1);
    CHECK(app_icon_draw("not-an-app.txt",10,20,40)==0);
    CHECK(app_icon_draw("bad name.aex",10,20,40)==0);
    printf("Raster icon loader: %d failure(s)\n",failures);
    return failures ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory(prefix='settings-icons-') as work:
    work = pathlib.Path(work)
    for name, contents in headers.items():
        (work / name).write_text(contents)
    source = (root / 'c/kernel/gui/app_icons.c').read_text()
    if negative:
        source = source.replace('fb_blit_rgba(x, y, size, size, image->rgba, image->w, image->h);', '(void)x; (void)y; (void)size;')
    (work / 'app_icons.c').write_text(source)
    (work / 'test.c').write_text(test)
    command = ['clang', '-Wall', '-Wextra', '-fsanitize=address,undefined',
               '-I' + str(work), '-I' + str(root / 'c/kernel/gui'), '-I' + str(root / 'include/abi'),
               str(work / 'app_icons.c'), str(work / 'test.c'), '-o', str(work / 'test')]
    subprocess.run(command, check=True)
    result = subprocess.run([str(work / 'test')], capture_output=True, text=True)
    print(result.stdout, end='')
    if negative:
        if result.returncode != 1 or 'FAIL draws==1' not in result.stdout:
            sys.exit('FAIL raster control did not detect a missing draw')
        print('PASS negative control: decoded but unpainted artwork fails')
    elif result.returncode:
        sys.exit('FAIL raster icon loader')
