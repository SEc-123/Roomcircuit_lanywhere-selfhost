#!/usr/bin/env python3
"""Run actual vendored list, glyph-cache and input lifecycle methods at host boundaries."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE = (ROOT / 'console/libtesla/include/tesla.hpp').read_text()


def block(signature):
    start = SOURCE.index(signature)
    opening = SOURCE.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[start:end]


def run(name, source):
    with tempfile.TemporaryDirectory(prefix='tesla-' + name + '-') as temporary:
        directory = Path(temporary)
        code = directory / 'test.cpp'
        exe = directory / 'test'
        code.write_text(source)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
            '-D_GLIBCXX_DEBUG', '-fsanitize=undefined', '-fno-sanitize-recover=all',
            str(code), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=10)


preamble = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;using s32=int32_t;
'''
list_fixture = preamble + r'''
enum class InputMode {Controller,TouchScroll};
enum class TouchEvent {Release,Move};
enum class FocusDirection {None,Up,Down};
namespace tsl::style::color {constexpr int ColorHandle=0;}
int a(int color){return color;}
namespace gfx {struct Renderer {template<class...T>void drawRect(T...){ }template<class...T>void drawCircle(T...){ }template<class...T>void enableScissoring(T...){ }void disableScissoring(){} };}
struct Element {
 virtual ~Element()=default;
 virtual void draw(gfx::Renderer*){}virtual void layout(u16,u16,u16,u16){}
 virtual bool onTouch(TouchEvent,s32,s32,s32,s32,s32,s32){return false;}
 virtual Element*requestFocus(Element*,FocusDirection){return this;}
 int x=0,y=0,width=300,height=50;
 void setBoundaries(int a,int b,int c,int d){x=a;y=b;width=c;height=d;}
 int getX()const{return x;}int getY()const{return y;}int getWidth()const{return width;}int getHeight()const{return height;}
 int getTopBound()const{return y;}int getBottomBound()const{return y+height;}int getLeftBound()const{return x;}int getRightBound()const{return x+width;}
 bool inBounds(int,int)const{return true;}void setParent(Element*){}void invalidate(){}void frame(gfx::Renderer*){}
 InputMode getInputMode()const{return InputMode::Controller;}
};
''' + block('class List : public Element') + r''';
struct Tracked:Element {
 static inline int live=0;
 bool focusable=true;
 explicit Tracked(bool enabled=true):focusable(enabled){live++;height=90;}
 ~Tracked(){live--;}
 Element*requestFocus(Element*,FocusDirection)override{return focusable?this:nullptr;}
};
struct TestList:List {
 size_t selected()const{return m_focusedIndex;}size_t size()const{return m_items.size();}
 void overflowing(){m_listHeight=900;m_offset=35;}
};
int main(){
 gfx::Renderer renderer;
 {
  TestList list;
  assert(list.requestFocus(nullptr,FocusDirection::None)==nullptr);
  auto*first=new Tracked;list.addItem(first);list.draw(&renderer);list.setFocusedIndex(0);list.overflowing();
  list.removeItem(first);list.draw(&renderer);assert(list.size()==0&&list.selected()==0&&Tracked::live==0);
  auto*a=new Tracked(false);auto*b=new Tracked;auto*c=new Tracked;
  list.addItem(a);list.addItem(b);list.addItem(c);list.draw(&renderer);list.setFocusedIndex(1);
  auto*inserted=new Tracked;list.addItem(inserted,90,0);list.draw(&renderer);
  assert(list.selected()==2&&list.getItemAtIndex(2)==b);
  list.removeItem(inserted);list.draw(&renderer);assert(list.selected()==1&&list.getItemAtIndex(1)==b);
  assert(list.requestFocus(b,FocusDirection::Up)==b); // No focus target above index zero.
  list.setFocusedIndex(2);list.removeItem(c);list.draw(&renderer);assert(list.selected()==1);
  list.removeItem(b);list.draw(&renderer);assert(list.selected()==0);
  list.clear();list.draw(&renderer);assert(list.size()==0&&list.selected()==0);
  assert(list.requestFocus(nullptr,FocusDirection::None)==nullptr);
  list.addItem(new Tracked); // Pending additions are owned even without a draw.
 }
 assert(Tracked::live==0);
 puts("Tesla list: empty, first/last removal, insertion focus, no focusable predecessor, clear and pending ownership passed");
}
'''
run('list', list_fixture)

cache_begin = SOURCE.index('            struct Glyph {')
cache_end = SOURCE.index('            /**', cache_begin)
cache = SOURCE[cache_begin:cache_end]
glyph_fixture = preamble + r'''
struct stbtt_fontinfo{int id=0;};
static size_t live_bytes=0,peak_bytes=0,allocations=0;
static int glyph_width=24,glyph_height=24;
static bool allocation_fails=false;
int stbtt_FindGlyphIndex(stbtt_fontinfo*font,u32 cp){
 if(cp>=0xe000)return font->id==3;
 if(cp<128)return font->id==1;
 if(cp>=0xac00&&cp<0xd800)return font->id==2;
 return font->id==4;
}
float stbtt_ScaleForPixelHeight(stbtt_fontinfo*,float size){return size/20.0F;}
void stbtt_GetCodepointBitmapBoxSubpixel(stbtt_fontinfo*,u32,float,float,float,float,int*a,int*b,int*c,int*d){*a=*b=0;*c=glyph_width;*d=glyph_height;}
void stbtt_GetCodepointHMetrics(stbtt_fontinfo*,u32,int*a,int*b){*a=10;*b=0;}
u8*stbtt_GetCodepointBitmap(stbtt_fontinfo*,float,float,u32,int*w,int*h,int*,int*){
 if(allocation_fails)return nullptr;
 *w=glyph_width;*h=glyph_height;const size_t bytes=size_t(*w)*size_t(*h);
 auto*allocation=static_cast<size_t*>(std::malloc(bytes+sizeof(size_t)));assert(allocation);*allocation=bytes;
 live_bytes+=bytes;peak_bytes=std::max(peak_bytes,live_bytes);allocations++;
 return reinterpret_cast<u8*>(allocation+1);
}
void stbtt_FreeBitmap(u8*p,void*){auto*base=reinterpret_cast<size_t*>(p)-1;live_bytes-=*base;std::free(base);}
struct Cache {
 stbtt_fontinfo m_stdFont{1},m_localFont{2},m_extFont{3},m_chineseFont{4};
 bool m_hasLocalFont=true,m_hasChineseFont=true;
''' + cache + r'''
};
int main(){
 Cache cache;
 assert(cache.fontForCodepoint('A')->id==1);
 assert(cache.fontForCodepoint(0xac01)->id==2);
 assert(cache.fontForCodepoint(0x4e00)->id==4);
 assert(cache.fontForCodepoint(0xe001)->id==3);
 cache.m_hasLocalFont=false;assert(cache.fontForCodepoint(0x4e00)->id==4);
 for(unsigned i=0;i<2000;i++)cache.cachedGlyph(i,i,false,20);
 assert(live_bytes<=Cache::GlyphBitmapBudget&&peak_bytes<=Cache::GlyphBitmapBudget);
 const auto count=allocations;cache.cachedGlyph(1999,1999,false,20);assert(allocations==count);
 cache.clearGlyphCache();assert(live_bytes==0&&cache.m_glyphBytes==0);
 glyph_width=glyph_height=512;
 cache.cachedGlyph(1,1,false,20);cache.cachedGlyph(2,2,false,20);
 assert(live_bytes==Cache::GlyphBitmapBudget);
 glyph_width=513;cache.cachedGlyph(3,3,false,20);assert(live_bytes==Cache::GlyphBitmapBudget);
 allocation_fails=true;glyph_width=glyph_height=24;
 assert(cache.cachedGlyph(4,4,false,20)->glyphBmp==nullptr);
 allocation_fails=false;assert(cache.cachedGlyph(4,4,false,20)->glyphBmp!=nullptr);
 cache.clearGlyphCache();assert(live_bytes==0);
 puts("Tesla glyph cache: bounded entries/bytes, eviction/release, allocation recovery and CJK fallback passed");
}
'''
run('glyph', glyph_fixture)

thread_fixture = preamble + r'''
using Result=int;
#define R_FAILED(r) ((r)!=0)
struct Event{int handle=0;};struct Thread{int handle=0;};
struct SharedThreadData{std::atomic<bool>running{false};Event comboEvent;};
static int failure=0,events=0,threads=0;static bool started=false,joined=false;
void backgroundEventPoller(void*){}
Result eventCreate(Event*e,bool){if(failure==1)return 10;e->handle=1;events++;return 0;}
void eventClose(Event*e){assert(e->handle&&(!started||joined));e->handle=0;events--;}
Result threadCreate(Thread*t,void(*)(void*),void*arg,void*,size_t stack,int,int){
 assert(static_cast<SharedThreadData*>(arg)->comboEvent.handle&&stack>=0x4000);
 if(failure==2)return 20;
 t->handle=1;threads++;return 0;
}
Result threadStart(Thread*t){assert(t->handle);if(failure==3)return 30;started=true;return 0;}
void threadWaitForExit(Thread*t){assert(t->handle&&started&&events==1);joined=true;}
void threadClose(Thread*t){assert(t->handle&&(!started||joined));t->handle=0;threads--;}
''' + block('static Result startInputThread(') + '\n' + block('static void stopInputThread(') + r'''
int main(){
 for(failure=1;failure<=3;failure++){
  SharedThreadData data;Thread thread{};started=joined=false;
  assert(startInputThread(data,thread)==failure*10&&!data.running&&events==0&&threads==0);
 }
 failure=0;SharedThreadData data;Thread thread{};
 assert(startInputThread(data,thread)==0&&data.running&&events==1&&threads==1);
 stopInputThread(data,thread);assert(!data.running&&events==0&&threads==0);
 puts("Tesla input lifecycle: create/start failures release exactly owned handles; join precedes event close");
}
'''
run('thread', thread_fixture)

assert 'std::atomic<bool> running{false}' in SOURCE
assert 'std::atomic<bool> overlayOpen{false}' in SOURCE
assert 'tsl::Overlay::get()->hide()' not in block('static void backgroundEventPoller(')
assert 'PlSharedFontType_ChineseSimplified' in block('Result initFonts(')
assert 'clearGlyphCache' in block('Result initFonts(')
print('Actual vendored methods tested with host gfx/font/kernel boundaries; physical overlay acceptance remains pending.')
