#include <stdio.h>
#include <string.h>
#include "httpd_protocol.h"
#include "ssh_conn.h"
#include "ssh.h"
static int checks, failures;
#define CHECK(c, label) do { checks++; if (!(c)) { failures++; printf("FAIL %s\n", label); } } while (0)
int main(void)
{
    long first, count;
    CHECK(hd_range("bytes=10-19", 100, &first, &count)==1 && first==10 && count==10, "bounded range");
    CHECK(hd_range("bytes=90-", 100, &first, &count)==1 && first==90 && count==10, "open range");
    CHECK(hd_range("bytes=-8", 100, &first, &count)==1 && first==92 && count==8, "suffix range");
    CHECK(hd_range("bytes=90-150", 100, &first, &count)==1 && first==90 && count==10, "end clamping");
    CHECK(hd_range("bytes=-150", 100, &first, &count)==1 && first==0 && count==100, "long suffix");
    CHECK(hd_range("bytes=100-", 100, &first, &count)==-1, "unsatisfiable range");
    CHECK(hd_range("bytes=0-", 0, &first, &count)==-1, "empty representation");
    CHECK(hd_range("items=1-2", 100, &first, &count)==0, "unknown units get full response");
    CHECK(hd_range("bytes=0-1,8-9", 100, &first, &count)==0, "multipart fallback");
    char path[256], value[128];
    CHECK(safe_path("/www", "/", path, sizeof path)==0 && !strcmp(path,"/www/index.html"), "index path");
    CHECK(safe_path("/download/", "/report%20one.json?version=1", path, sizeof path)==0 && !strcmp(path,"/download/report one.json"), "encoded file name");
    CHECK(safe_path("/www", "/%E6%8A%A5%E5%91%8A.txt", path, sizeof path)==0 && !strcmp(path,"/www/报告.txt"), "UTF8 file name");
    CHECK(!strcmp(mime_of("/a.WASM"),"application/wasm"), "wasm MIME");
    CHECK(!strcmp(mime_of("/a.mp4"),"video/mp4"), "video MIME");
    CHECK(!strcmp(mime_of("/a.unknown"),"application/octet-stream"), "binary MIME fallback");
    CHECK(hd_header("GET / HTTP/1.1\r\nrAnGe: bytes=0-9\t\r\n\r\n","Range",value,sizeof value) && !strcmp(value,"bytes=0-9"), "case insensitive header");
    unsigned char pkt[64];
    int n=ssh_build_channel_stderr(7,(const unsigned char *)"err",3,pkt,sizeof pkt);
    const unsigned char expected[]={95,0,0,0,7,0,0,0,1,0,0,0,3,'e','r','r'};
    CHECK(n==sizeof expected && !memcmp(pkt,expected,sizeof expected), "RFC stderr wire bytes");
    printf("SERVER_SEMANTICS checks=%d failures=%d\n",checks,failures);
    return failures != 0;
}
