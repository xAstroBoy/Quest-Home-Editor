// ── ui_core.h — immediate-mode widget toolkit + Blender-faithful dark theme (the custom editor UI) ──────────
// Rect-based immediate mode over ui_draw.h: the editor spaces compute layout (Blender panels are grid-laid),
// call widgets with explicit rects, and read back interaction. Input is fed from GLFW (editor routes callbacks).
#pragma once
#include "ui/ui_draw.h"
#include <string>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <vector>
#include <functional>

namespace ui {

// GLFW key codes used by editable fields (match GLFW3 values; editor fills keyPressed[glfwKey]).
enum { KEY_ESCAPE=256, KEY_ENTER=257, KEY_TAB=258, KEY_BACKSPACE=259, KEY_DELETE=261,
       KEY_RIGHT=262, KEY_LEFT=263, KEY_HOME=268, KEY_END=269, KEY_A=65, KEY_C=67, KEY_V=86, KEY_X=88, KEY_Z=90 };

struct Input {
    float mx=0, my=0, dmx=0, dmy=0, wheel=0;
    bool  down[3]={}, pressed[3]={}, released[3]={};   // 0=L 1=R 2=M
    double pressX[3]={}, pressY[3]={};
    bool  keyDown[400]={}, keyRepeat[400]={};
    bool  shift=false, ctrl=false, alt=false;
    std::string text;                                   // chars typed this frame
    void newFrame() { dmx=dmy=0; wheel=0; for(int i=0;i<3;i++){pressed[i]=released[i]=false;} memset(keyRepeat,0,sizeof keyRepeat); text.clear(); }
};

struct Theme {                                          // "Slate" — modern dark blue-gray editor palette (professional pass)
    uint32_t areaBg    = rgba(24,26,32);
    uint32_t headerBg  = rgba(32,34,42);
    uint32_t panelBg   = rgba(40,42,52);
    uint32_t subPanel  = rgba(34,36,45);
    uint32_t text      = rgba(224,228,236);
    uint32_t textDim   = rgba(148,155,170);
    uint32_t textSel   = rgba(255,255,255);
    uint32_t widget    = rgba(58,62,76);                // button / number field
    uint32_t widgetHot = rgba(74,80,98);
    uint32_t widgetDown= rgba(90,98,120);
    uint32_t field     = rgba(24,26,33);                // text/number field inset
    uint32_t accent    = rgba(64,140,235);              // select blue
    uint32_t accentHot = rgba(88,158,245);
    uint32_t toggleOn  = rgba(64,140,235);
    uint32_t border    = rgba(16,17,22);
    uint32_t rowSel    = rgba(64,140,235,110);
    uint32_t rowHover  = rgba(255,255,255,16);
    uint32_t splitLine = rgba(14,15,19);
    float rowH = 22.f, headerH = 27.f, pad = 6.f, indent = 16.f;
};

inline uint32_t hashId(const char* s, uint32_t seed=2166136261u) {
    for (; s && *s; ++s) { seed ^= (uint8_t)*s; seed *= 16777619u; }
    return seed ? seed : 1u;
}
inline uint32_t hashId(uint32_t a, uint32_t salt) { a ^= salt + 0x9e3779b9u + (a<<6) + (a>>2); return a?a:1u; }

struct Context {
    DrawList* dl=nullptr; Font* font=nullptr; Font* mono=nullptr; Theme th;
    Input in;
    // system clipboard bridge for text fields (Ctrl+C/X/V) — bound by the editor to GLFW's clipboard
    std::function<void(const char*)> setClip; std::function<std::string()> getClip;
    uint32_t hot=0, active=0;        // hovered / pressed widget
    uint32_t kbFocus=0;              // text field with keyboard focus
    std::string editBuf; int editCur=0, editSel=-1;   // text-field edit state (editSel = selection anchor; -1 = no selection)
    float t=0.f; bool consumedMouse=false;

    bool inClip(float x,float y) const { VkRect2D c=dl->cur(); return x>=c.offset.x&&y>=c.offset.y&&x<c.offset.x+(int)c.extent.width&&y<c.offset.y+(int)c.extent.height; }
    bool hover(float x,float y,float w,float h) const { return in.mx>=x&&in.my>=y&&in.mx<x+w&&in.my<y+h && inClip(in.mx,in.my); }
    void textAligned(float x,float y,float w,float h,const char* s,uint32_t col,int align=0,Font* f=nullptr){ // 0=L 1=C 2=R
        Font* of=dl->font; if(f) dl->font=f;
        float tw=dl->textW(s); float tx = align==1 ? x+(w-tw)*0.5f : align==2 ? x+w-tw-4 : x+4;
        float ty = y+(h-(dl->font->ascent-dl->font->descent))*0.5f;
        dl->pushClip(x,y,w,h); dl->text(tx,ty,s,col); dl->popClip(); dl->font=of;
    }

    // ── widgets (rect-based; id = caller-provided stable hash) ──
    void label(float x,float y,float w,float h,const char* s,uint32_t col=0,int align=0){ textAligned(x,y,w,h,s, col?col:th.text, align); }

    bool button(uint32_t id,float x,float y,float w,float h,const char* s,bool accent=false){
        bool hv=hover(x,y,w,h); if(hv){hot=id; if(in.pressed[0]) active=id;}
        bool clicked=false; if(active==id&&in.released[0]){ if(hv)clicked=true; active=0; }
        uint32_t bg = accent? (active==id&&hv?th.accentHot:th.accent) : (active==id&&hv?th.widgetDown:hv?th.widgetHot:th.widget);
        dl->rect(x,y,w,h,bg); dl->border(x,y,w,h,th.border);
        textAligned(x,y,w,h,s, accent?th.textSel:th.text, 1);
        return clicked;
    }
    bool toggle(uint32_t id,float x,float y,float w,float h,const char* s,bool& v){
        bool hv=hover(x,y,w,h); if(hv){hot=id; if(in.pressed[0]) active=id;}
        bool clicked=false; if(active==id&&in.released[0]){ if(hv){clicked=true; v=!v;} active=0; }
        uint32_t bg = v? (hv?th.accentHot:th.accent) : (active==id&&hv?th.widgetDown:hv?th.widgetHot:th.widget);
        dl->rect(x,y,w,h,bg); dl->border(x,y,w,h,th.border);
        textAligned(x,y,w,h,s, v?th.textSel:th.text, 1);
        return clicked;
    }
    // Horizontal drag slider [lo,hi]. Click/drag the track sets the value; returns true while the value CHANGES.
    // The filled portion is drawn in `fill` (0 = theme accent) so RGB sliders can tint themselves.
    bool slider(uint32_t id,float x,float y,float w,float h,float& v,float lo,float hi,uint32_t fill=0){
        bool hv=hover(x,y,w,h); if(hv){hot=id; if(in.pressed[0]) active=id;}
        bool changed=false;
        if(active==id && in.down[0]){
            float t=(in.mx-x)/(w>1?w:1); if(t<0)t=0; if(t>1)t=1;
            float nv=lo+t*(hi-lo); if(nv!=v){ v=nv; changed=true; }
        }
        if(active==id && in.released[0]) active=0;
        float t=(hi>lo)?(v-lo)/(hi-lo):0.f; if(t<0)t=0; if(t>1)t=1;
        dl->rect(x,y+h*0.5f-3,w,6, th.field); dl->border(x,y+h*0.5f-3,w,6,th.border);   // track
        dl->rect(x,y+h*0.5f-3,w*t,6, fill?fill:th.accent);                              // fill
        float kx=x+w*t; dl->rect(kx-4,y+h*0.5f-8,8,16, hv||active==id?th.textSel:th.text); dl->border(kx-4,y+h*0.5f-8,8,16,th.border);  // knob
        return changed;
    }
    // Solid color swatch (click = returns true, for "make this the picked color" hooks). Draws a checker under it so
    // near-black vs unset reads clearly. r/g/b are 0..1.
    bool swatch(uint32_t id,float x,float y,float w,float h,float r,float g,float b){
        bool hv=hover(x,y,w,h); if(hv){hot=id; if(in.pressed[0]) active=id;}
        bool clicked=false; if(active==id&&in.released[0]){ if(hv)clicked=true; active=0; }
        auto c8=[](float f){ int i=(int)(f*255.f+0.5f); return (uint32_t)(i<0?0:i>255?255:i); };
        dl->rect(x,y,w,h, rgba((int)c8(r),(int)c8(g),(int)c8(b))); dl->border(x,y,w,h, hv?th.accent:th.border);
        return clicked;
    }
    bool checkbox(uint32_t id,float x,float y,const char* s,bool& v){
        float b=14.f, h=th.rowH;
        Font* fnt=mono?mono:font; float lw=b+8+(fnt?fnt->textWidth(s,(int)strlen(s)):320.f);   // full box+label width
        bool hv=hover(x,y,lw,h);                                  // the WHOLE row toggles (not just the tiny box)
        if(hv){hot=id; if(in.pressed[0]) active=id;}
        bool clicked=false; if(active==id&&in.released[0]){ if(hv){clicked=true;v=!v;} active=0; }
        float by=y+(h-b)*0.5f; dl->rect(x,by,b,b, th.field); dl->border(x,by,b,b,th.border);
        if(v){ dl->line(x+3,by+7,x+6,by+10,th.accent,2); dl->line(x+6,by+10,x+11,by+3,th.accent,2); }
        textAligned(x+b+5,y,lw,h,s,th.text,0);                    // width = the measured label -> never clipped
        return clicked;
    }
    // Shared single-line edit-key handling for EVERY focused input field (numeric + text), so Ctrl+A / Delete / Home /
    // End / Shift-select / highlight behave the same everywhere. Mutates editBuf/editCur/editSel. numeric=true filters
    // typed chars to digits . - + e E. Returns true the frame Enter is pressed (the caller commits/blurs).
    bool editKeys(bool numeric){
        auto delSel=[&]()->bool{ if(editSel<0||editSel==editCur){ editSel=-1; return false; }
            int a=editSel<editCur?editSel:editCur, b=editSel<editCur?editCur:editSel; editBuf.erase(a,b-a); editCur=a; editSel=-1; return true; };
        auto moveTo=[&](int np){ if(np<0)np=0; if(np>(int)editBuf.size())np=(int)editBuf.size();
            if(in.shift){ if(editSel<0) editSel=editCur; } else editSel=-1; editCur=np; };   // Shift = extend the selection
        auto selText=[&]()->std::string{ if(editSel<0||editSel==editCur) return editBuf;    // no selection -> whole field
            int a=editSel<editCur?editSel:editCur, b=editSel<editCur?editCur:editSel; return editBuf.substr(a,b-a); };
        if(in.ctrl && in.keyRepeat[KEY_A]){ editSel=0; editCur=(int)editBuf.size(); }         // Ctrl+A = select all (highlight)
        // ── system clipboard in text fields: Ctrl+C copy / Ctrl+X cut / Ctrl+V paste ──
        if(in.ctrl && in.keyRepeat[KEY_C] && setClip) setClip(selText().c_str());
        if(in.ctrl && in.keyRepeat[KEY_X] && setClip){ setClip(selText().c_str());
            if(!delSel()){ editBuf.clear(); editCur=0; editSel=-1; } }                        // no selection -> cut the whole field
        if(in.ctrl && in.keyRepeat[KEY_V] && getClip){ std::string cp=getClip();
            std::string ins; for(char c:cp){ if(c=='\r'||c=='\n') continue; if(!numeric || (c>='0'&&c<='9')||c=='.'||c=='-'||c=='+'||c=='e'||c=='E') ins.push_back(c); }
            if(!ins.empty()){ delSel(); editBuf.insert(editCur,ins); editCur+=(int)ins.size(); } }
        if(!in.text.empty() && !in.ctrl){ std::string ins; for(char c:in.text){ if(!numeric || (c>='0'&&c<='9')||c=='.'||c=='-'||c=='+'||c=='e'||c=='E') ins.push_back(c); }
            if(!ins.empty()){ delSel(); editBuf.insert(editCur,ins); editCur+=(int)ins.size(); } }
        if(in.keyRepeat[KEY_BACKSPACE]){ if(!delSel() && editCur>0){ editBuf.erase(editCur-1,1); editCur--; } }
        if(in.keyRepeat[KEY_DELETE]){ if(!delSel() && editCur<(int)editBuf.size()){ editBuf.erase(editCur,1); } }   // forward delete
        if(in.keyRepeat[KEY_LEFT])  moveTo(editCur-1);
        if(in.keyRepeat[KEY_RIGHT]) moveTo(editCur+1);
        if(in.keyRepeat[KEY_HOME])  moveTo(0);
        if(in.keyRepeat[KEY_END])   moveTo((int)editBuf.size());
        return in.keyRepeat[KEY_ENTER];
    }
    // Draw the current selection highlight + caret for a focused field (editBuf/editCur/editSel), scrolled by `scr`.
    void editDecorate(float x,float y,float w,float h,Font* fnt,float scr){
        if(!fnt) return; float pad=4.f;
        if(editSel>=0 && editSel!=editCur){ int a=editSel<editCur?editSel:editCur, b=editSel<editCur?editCur:editSel;
            float sa=fnt->textWidth(editBuf.c_str(),a), sb=fnt->textWidth(editBuf.c_str(),b);
            dl->rect(x+pad-scr+sa, y+3, sb-sa, h-6, th.accent); }
    }
    // Drag-to-edit numeric field (Blender-style). Returns true if value changed.
    // Numeric field: DRAG to scrub, or CLICK to type a value with the keyboard (Enter/Tab/click-away commits, Esc cancels).
    bool dragFloat(uint32_t id,float x,float y,float w,float h,float& v,float speed=0.01f,const char* fmt="%.3f"){
        bool hv=hover(x,y,w,h);
        bool changed=false;
        // ── keyboard entry mode (this field is focused) ──
        if(kbFocus==id){
            bool enter = editKeys(true);                              // Ctrl+A/Del/Home/End/select all share editKeys now
            bool commit = enter||in.keyRepeat[KEY_TAB]||(in.pressed[0]&&!hv);
            bool cancel = in.keyRepeat[KEY_ESCAPE];
            if(commit){ try{ v=std::stof(editBuf); changed=true; }catch(...){} kbFocus=0; }
            else if(cancel) kbFocus=0;
            dl->rect(x,y,w,h,th.field); dl->border(x,y,w,h,th.accent);
            Font* fnt=mono?mono:font;
            if(fnt){ float pad=4.f, avail=w-2*pad, cw=fnt->textWidth(editBuf.c_str(),editCur), scr=(cw>avail)?cw-avail:0.f;
                dl->pushClip(x,y,w,h); Font* of=dl->font; dl->font=fnt; float ty=y+(h-(fnt->ascent-fnt->descent))*0.5f;
                editDecorate(x,y,w,h,fnt,scr);                        // selection highlight
                dl->text(x+pad-scr,ty,editBuf.c_str(),th.text);
                if(((int)(t*2))&1) dl->rect(x+pad-scr+cw,y+3,1,h-6,th.text);
                dl->font=of; dl->popClip(); }
            return changed;
        }
        // ── drag to scrub, or click (no drag) to focus for typing ──
        if(hv){hot=id; if(in.pressed[0]) active=id;}
        if(active==id){
            if(in.down[0]){ if(in.dmx!=0){ v+=in.dmx*speed; changed=true; } }
            else { float ddx=in.mx-in.pressX[0], ddy=in.my-in.pressY[0];
                   if(ddx*ddx+ddy*ddy < 16.f){ kbFocus=id; char b[40]; snprintf(b,sizeof b,"%.5g",v); editBuf=b; editCur=(int)editBuf.size(); }  // a click -> type
                   active=0; }
        }
        uint32_t bg = active==id?th.widgetDown:hv?th.widgetHot:th.field;
        dl->rect(x,y,w,h,bg); dl->border(x,y,w,h,th.border);
        char b[64]; snprintf(b,sizeof b,fmt,v); textAligned(x,y,w,h,b, th.text, 1, mono);
        return changed;
    }
    void progressBar(float x,float y,float w,float h,float frac,const char* label=nullptr){
        if(frac<0)frac=0; if(frac>1)frac=1;
        dl->rect(x,y,w,h,th.field); dl->border(x,y,w,h,th.border);
        dl->rect(x+1,y+1,(w-2)*frac,h-2, th.accent);
        if(label) textAligned(x,y,w,h,label, th.textSel,1);
    }
    // Tab in a tab bar.
    bool tab(uint32_t id,float x,float y,float w,float h,const char* s,bool sel){
        bool hv=hover(x,y,w,h); if(hv){hot=id; if(in.pressed[0]) active=id;}
        bool clicked=false; if(active==id&&in.released[0]){ if(hv)clicked=true; active=0; }
        dl->rect(x,y,w,h, sel?th.panelBg : hv?th.widgetHot:th.headerBg);
        if(sel) dl->rect(x,y+h-2,w,2,th.accent);
        textAligned(x,y,w,h,s, sel?th.textSel:th.textDim, 1);
        return clicked;
    }
    // Collapsing panel header: draws a triangle + title; toggles `open`. Returns `open`.
    bool collapse(uint32_t id,float x,float y,float w,const char* s,bool& open){
        float h=th.rowH+2; bool hv=hover(x,y,w,h); if(hv){hot=id; if(in.pressed[0])active=id;}
        if(active==id&&in.released[0]){ if(hv)open=!open; active=0; }
        dl->rect(x,y,w,h, hv?th.widgetHot:th.subPanel);
        float cx=x+10,cy=y+h*0.5f;
        if(open) dl->triangle(cx-4,cy-2,cx+4,cy-2,cx,cy+4,th.textDim);
        else     dl->triangle(cx-2,cy-4,cx+4,cy,cx-2,cy+4,th.textDim);
        textAligned(x+20,y,w-20,h,s,th.text,0);
        return open;
    }
    // Outliner-style selectable row with an expand triangle (hasChild) + visibility eye.
    struct RowResult { bool clicked=false, toggledOpen=false, toggledVis=false; };
    RowResult treeRow(uint32_t id,float x,float y,float w,float h,const char* s,int depth,bool hasChild,bool open,bool sel,bool visible){
        RowResult rr; bool hv=hover(x,y,w,h);
        if(sel) dl->rect(x,y,w,h,th.rowSel); else if(hv) dl->rect(x,y,w,h,th.rowHover);
        float ix=x+4+depth*th.indent;
        if(hasChild){ float cx=ix+5,cy=y+h*0.5f;
            bool th_hv=hover(ix,y,14,h);
            if(open) dl->triangle(cx-4,cy-2,cx+4,cy-2,cx,cy+4,th.textDim);
            else dl->triangle(cx-2,cy-4,cx+4,cy,cx-2,cy+4,th.textDim);
            if(th_hv&&in.pressed[0]){ rr.toggledOpen=true; } }
        // visibility EYE at the right (Blender-style: open almond + pupil = shown, flat line = hidden) — row-scaled
        float er=h*0.26f, ecx=x+w-er-7.f, ecy=y+h*0.5f;
        textAligned(ix+16,y, (x+w-2*er-12)-(ix+16), h, s, sel?th.textSel:th.text, 0);   // reserve room for the eye
        bool eye_hv=hover(ecx-er-3,y,2*er+10,h);
        uint32_t ecol = visible?th.text:th.textDim;
        if(visible){ float px=ecx-er,pu=ecy,pl=ecy;
            for(int s2=1;s2<=8;s2++){ float t=s2/8.f, axp=ecx-er+2*er*t, dy=er*0.62f*sinf(3.14159265f*t);
                dl->line(px,pu,axp,ecy-dy,ecol,1.2f); dl->line(px,pl,axp,ecy+dy,ecol,1.2f); px=axp;pu=ecy-dy;pl=ecy+dy; }
            dl->rect(ecx-1.6f,ecy-1.6f,3.2f,3.2f,ecol);
        } else dl->line(ecx-er,ecy,ecx+er,ecy,ecol,1.6f);
        if(eye_hv&&in.pressed[0]) rr.toggledVis=true;
        else if(hv&&in.pressed[0]&&!rr.toggledOpen) rr.clicked=true;
        return rr;
    }
    // Editable single-line text field (used by Cook package name).
    bool textField(uint32_t id,float x,float y,float w,float h,std::string& s){
        bool hv=hover(x,y,w,h); if(hv&&in.pressed[0]){ kbFocus=id; editBuf=s; editCur=(int)s.size(); editSel=-1; }
        if(in.pressed[0]&&!hv&&kbFocus==id){ kbFocus=0; s=editBuf; }
        bool committed=false;
        if(kbFocus==id){
            if(editKeys(false)){ s=editBuf; kbFocus=0; committed=true; }   // Enter commits; Ctrl+A/Del/Home/End/select shared
            if(in.keyRepeat[KEY_ESCAPE]){ kbFocus=0; }
        }
        const std::string& shown = (kbFocus==id)?editBuf:s;
        dl->rect(x,y,w,h, kbFocus==id?th.field:hv?th.widgetHot:th.field); dl->border(x,y,w,h, kbFocus==id?th.accent:th.border);
        Font* fnt = mono?mono:font; if(!fnt) return committed;
        float pad=4.f, avail=w-2*pad;
        float caretX = fnt->textWidth(shown.c_str(), (kbFocus==id)?editCur:(int)shown.size());
        float scroll = (caretX>avail) ? caretX-avail : 0.f;          // keep the caret in view (horizontal scroll)
        dl->pushClip(x,y,w,h);                                        // EVERYTHING (text + caret) clipped to the box
        Font* of=dl->font; dl->font=fnt;
        float ty=y+(h-(fnt->ascent-fnt->descent))*0.5f;
        if(kbFocus==id && editSel>=0 && editSel!=editCur){            // selection highlight (Ctrl+A etc.)
            int a=editSel<editCur?editSel:editCur, b=editSel<editCur?editCur:editSel;
            float sa=fnt->textWidth(shown.c_str(),a), sb=fnt->textWidth(shown.c_str(),b);
            dl->rect(x+pad-scroll+sa, y+3, sb-sa, h-6, th.accent);
        }
        dl->text(x+pad-scroll, ty, shown.c_str(), th.text);
        if(kbFocus==id && (((int)(t*2))&1)) dl->rect(x+pad-scroll+caretX, y+3, 1, h-6, th.text);
        dl->font=of;
        dl->popClip();
        return committed;
    }

    // ── deferred tooltips ── call tip() right after a widget (same rect); call drawTooltip() ONCE at the very end
    // of the frame (after every panel/popup, with all clips popped) so the box renders on top and isn't clipped.
    std::string ttText; float ttX=0, ttY=0; bool ttShow=false;
    void tip(float x,float y,float w,float h,const char* s){ if(s && *s && hover(x,y,w,h)){ ttText=s; ttX=in.mx; ttY=in.my; ttShow=true; } }
    void drawTooltip(){
        if(!ttShow){ return; }
        ttShow=false;                                   // consume; re-armed next frame while still hovering
        Font* fnt = font?font:mono; if(!fnt||!dl){ return; }
        std::vector<std::string> lines; { const std::string& s=ttText; size_t i=0;
            while(true){ size_t nl=s.find('\n',i); if(nl==std::string::npos){ lines.push_back(s.substr(i)); break; } lines.push_back(s.substr(i,nl-i)); i=nl+1; } }
        float pad=6.f, lineH=(fnt->ascent-fnt->descent)+3.f, wmax=0;
        for(auto&l:lines){ float lw=fnt->textWidth(l.c_str(),(int)l.size()); if(lw>wmax)wmax=lw; }
        float bw=wmax+2*pad, bh=lines.size()*lineH+2*pad;
        VkRect2D root=dl->cur(); float W=(float)root.offset.x+root.extent.width, H=(float)root.offset.y+root.extent.height;
        float bx=ttX+14, by=ttY+18;
        if(bx+bw>W) bx=ttX-bw-6; if(bx<0)bx=0;          // flip left / clamp on-screen
        if(by+bh>H) by=H-bh;      if(by<0)by=0;
        dl->rect(bx,by,bw,bh, rgba(22,22,22,238)); dl->border(bx,by,bw,bh, th.accent);
        Font* of=dl->font; dl->font=fnt; float ty=by+pad+fnt->ascent;
        for(auto&l:lines){ dl->text(bx+pad,ty,l.c_str(),th.textSel); ty+=lineH; }
        dl->font=of;
    }
};

} // namespace ui
