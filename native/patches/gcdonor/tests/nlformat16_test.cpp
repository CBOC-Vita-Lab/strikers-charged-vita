// Host test for contrib/nlFormat16.h:  g++ -O1 -I<patches/gcdonor> -o /tmp/nf tests/nlformat16_test.cpp && /tmp/nf
#include <stdio.h>
#include <string.h>
#include "contrib/nlFormat16.h"
static void w(const char* s, unsigned short* out){ while((*out++=(unsigned short)(unsigned char)*s++)); }
static void n(const unsigned short* s, char* out){ while(*s) *out++=(char)*s++; *out=0; }
static int fails=0;
static int fmt16(unsigned short* o, unsigned long size, const unsigned short* f, ...)
{ va_list a; va_start(a,f); int r=nlFormat16(o,size,f,a); va_end(a); return r; }
#define CHECK(fmt, ...) do{ unsigned short f[128],o[256]; char got[256],exp[256]; w(fmt,f); int r=fmt16(o,256,f,__VA_ARGS__); n(o,got); int e=snprintf(exp,256,fmt,__VA_ARGS__); if(strcmp(got,exp)||r!=e){printf("FAIL %s: got '%s'(%d) exp '%s'(%d)\n",fmt,got,r,exp,e);fails++;} }while(0)
int main(){
  CHECK("%d",42); CHECK("%.2d",5); CHECK("%.4d",7); CHECK("0%d",9); CHECK("%5d|%-5d|",3,4); CHECK("%x %X %o",255,255,8);
  CHECK("%.6llu %.6llu",123ULL,456789012345ULL); CHECK("%ld",-5L); CHECK("%u",4000000000u); CHECK("%s!","abc"); CHECK("%5s|%-5s|","ab","cd");
  CHECK("%c%c",'x','y'); CHECK("%f",3.5); CHECK("%.1f",2.25); CHECK("%5.2f%%",3.14159); CHECK("%+d",3); CHECK("%*d",6,12);
  unsigned short f[16],o[16]; char got[64]; w("%ls|%d",f); unsigned short ws[4]={'h','i',0}; fmt16(o,16,f,ws,7); n(o,got); if(strcmp(got,"hi|7")){printf("FAIL ls: %s\n",got);fails++;}
  w("%d",f); int r=fmt16(o,4,f,123456); n(o,got); if(strcmp(got,"123")||r!=6){printf("FAIL trunc '%s' %d\n",got,r);fails++;}
  printf("done fails=%d\n",fails); return fails;
}
