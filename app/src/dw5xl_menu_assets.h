#pragma once
// Issue 023 step 3S: the game's own menu pictures for the PC SETTINGS screen, read from the player's disc files at
// run time (never stored in the repository). Formats worked out from DW5XL's data (notes: 023 work.md, step 1):
// LINKDATA.IDX entries, KOEI's LZP2 packing, TIM2 pictures, the small glyph font in the executable. On top of them:
// letters split by shape (each letter's fill plus the outline/shadow nearest to it) and words built from them, and
// the title font's glow learned from a title and regenerated around new words.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace dw5xl::menu {
struct Image {                                   // straight alpha, 0..255 per channel
    int w=0,h=0; std::vector<uint8_t> rgba;
    Image()=default;
    Image(int width,int height):w(width),h(height),rgba(size_t(width)*height*4,0) {}
    uint8_t* at(int x,int y) { return &rgba[(size_t(y)*w+x)*4]; }
    const uint8_t* at(int x,int y) const { return &rgba[(size_t(y)*w+x)*4]; }
};
inline uint32_t le32(const uint8_t* p) { return p[0]|p[1]<<8|p[2]<<16|uint32_t(p[3])<<24; }

// LINKDATA.IDX: "SM4L", u32 entry count, then 16-byte entries from +0x10 (start sector, sectors, size, flags).
struct LinkEntry { uint32_t sector=0,sectors=0,size=0,flags=0; uint64_t offset() const { return uint64_t(sector)*2048; } };
inline std::vector<LinkEntry> parseLinkIndex(const uint8_t* data,size_t n) {
    std::vector<LinkEntry> out;
    if(n<16 || std::memcmp(data,"SM4L",4)!=0) return out;
    const uint32_t count=le32(data+4);
    for(uint32_t i=0;i<count && 0x10+size_t(i+1)*16<=n;++i) {
        const uint8_t* e=data+0x10+size_t(i)*16;
        out.push_back({le32(e),le32(e+4),le32(e+8),le32(e+12)});
    }
    return out;
}

// LZP2: "LZP2", float 1.01, u32 unpacked size, u32 packed size; stream from +0x10. Token t:
//   t < 0x40          t literal bytes
//   0x40 <= t < 0x80  (+ n, b) ((((t & 0x3f) << 8) | n) + 4) bytes of value b
//   t >= 0x80         (+ n) ((t >> 3) & 15) + 3 bytes copied from (((t & 7) << 8) | n) + 1 back
inline bool unpackLzp2(const uint8_t* data,size_t n,std::vector<uint8_t>& out) {
    out.clear();
    if(n<16 || std::memcmp(data,"LZP2",4)!=0) return false;
    const uint32_t size=le32(data+8);
    out.reserve(size);
    size_t i=16;
    while(out.size()<size) {
        if(i>=n) return false;
        const uint8_t t=data[i++];
        if(t<0x40) {
            if(i+t>n || out.size()+t>size) return false;
            out.insert(out.end(),data+i,data+i+t); i+=t;
        } else if(t<0x80) {
            if(i+2>n) return false;
            const size_t count=((size_t(t&0x3f)<<8)|data[i])+4;
            if(out.size()+count>size) return false;
            out.insert(out.end(),count,data[i+1]); i+=2;
        } else {
            if(i>=n) return false;
            const size_t length=((t>>3)&15)+3,dist=((size_t(t&7)<<8)|data[i++])+1;
            if(dist>out.size() || out.size()+length>size) return false;
            for(size_t k=0;k<length;++k) out.push_back(out[out.size()-dist]);
        }
    }
    return true;
}

// TIM2 pictures (standard PS2 format) inside a container.
struct Tim2Picture { size_t pixels=0,clut=0; uint32_t imageSize=0,clutSize=0,colors=0; int w=0,h=0; uint8_t clutType=0,imageType=0; };
inline std::vector<Tim2Picture> findTim2(const uint8_t* data,size_t n) {
    std::vector<Tim2Picture> out;
    for(size_t p=0;p+16+48<=n;++p) {
        if(std::memcmp(data+p,"TIM2",4)!=0) continue;
        const size_t pic=p+(data[p+5]==0?16:128);
        if(pic+48>n) continue;
        const uint8_t* h=data+pic;
        Tim2Picture t;
        t.clutSize=le32(h+4); t.imageSize=le32(h+8);
        const uint16_t header=uint16_t(h[12]|h[13]<<8);
        t.colors=uint32_t(h[14]|h[15]<<8); t.clutType=h[18]; t.imageType=h[19];
        t.w=h[20]|h[21]<<8; t.h=h[22]|h[23]<<8;
        t.pixels=pic+header; t.clut=t.pixels+t.imageSize;
        if(t.clut+t.clutSize>n || (t.imageType!=4 && t.imageType!=5)) continue;
        out.push_back(t);
    }
    return out;
}
// Indexed TIM2 with a 32-bit palette. 4-bit pictures use palette block `palette` of 16 colours; 8-bit pictures use
// the PS2 palette order (entries 8..15 and 16..23 swapped in each 32) unless the TIM2 says it is already linear.
// PS2 alpha 0x80 is opaque.
inline Image decodeTim2(const uint8_t* data,size_t n,const Tim2Picture& t,uint32_t palette=0) {
    Image img(t.w,t.h);
    const uint32_t entries=t.imageType==4?16u:256u;
    const size_t base=t.clut+size_t(palette)*entries*4;
    if((t.clutType&0x3f)!=3 || base+entries*4>t.clut+t.clutSize || t.clut+t.clutSize>n) return {};
    const size_t needed=t.imageType==4?(size_t(t.w)*t.h+1)/2:size_t(t.w)*t.h;
    if(t.pixels+needed>n || needed>t.imageSize) return {};
    for(int y=0;y<t.h;++y) for(int x=0;x<t.w;++x) {
        const size_t k=size_t(y)*t.w+x;
        uint32_t index=t.imageType==4?((data[t.pixels+k/2]>>((k&1)*4))&15):data[t.pixels+k];
        if(t.imageType==5 && !(t.clutType&0x80)) index=(index&0xE7)|((index&0x08)<<1)|((index&0x10)>>1);
        const uint8_t* c=data+base+index*4; uint8_t* o=img.at(x,y);
        o[0]=c[0];o[1]=c[1];o[2]=c[2];o[3]=uint8_t(std::min(255,c[3]*2));
    }
    return img;
}
// The raw palette indices of an indexed TIM2 (one per pixel).
inline std::vector<uint8_t> tim2Indices(const uint8_t* data,size_t n,const Tim2Picture& t) {
    std::vector<uint8_t> out;
    const size_t needed=t.imageType==4?(size_t(t.w)*t.h+1)/2:size_t(t.w)*t.h;
    if(t.pixels+needed>n) return out;
    out.resize(size_t(t.w)*t.h);
    for(size_t k=0;k<out.size();++k) out[k]=t.imageType==4?((data[t.pixels+k/2]>>((k&1)*4))&15):data[t.pixels+k];
    return out;
}
// An RGBA picture fitted to an 8-bit TIM2's palette (nearest colour, alpha weighted like a colour channel); returns
// the indices as stored in the TIM2 (PS2 palette order). Fully transparent pixels take the most transparent entry.
inline std::vector<uint8_t> quantize(const Image& img,const uint8_t* data,const Tim2Picture& t) {
    std::vector<uint8_t> out(size_t(img.w)*img.h,0);
    if(t.imageType!=5 || t.clutSize<256*4) return {};
    auto stored=[&](int e){ return uint8_t((e&0xE7)|((e&0x08)<<1)|((e&0x10)>>1)); };   // order swap is its own inverse
    const bool linear=(t.clutType&0x80)!=0;
    int clearest=0;
    for(int e=0;e<256;++e) if(data[t.clut+e*4+3]<data[t.clut+clearest*4+3]) clearest=e;
    for(int y=0;y<img.h;++y) for(int x=0;x<img.w;++x) {
        const uint8_t* p=img.at(x,y); int best=clearest;
        if(p[3]>0) {
            long bestD=-1;
            for(int e=0;e<256;++e) {
                const uint8_t* c=data+t.clut+e*4; const int a=std::min(255,c[3]*2);
                const long d=long(c[0]-p[0])*(c[0]-p[0])+long(c[1]-p[1])*(c[1]-p[1])+long(c[2]-p[2])*(c[2]-p[2])+2L*(a-p[3])*(a-p[3]);
                if(bestD<0 || d<bestD) { bestD=d; best=e; }
            }
        }
        out[size_t(y)*img.w+x]=linear?uint8_t(best):stored(best);
    }
    return out;
}
// The small font in the executable: 160-byte glyphs (16x20, 4-bit indices), 16-colour 32-bit palette.
inline Image decodeGlyph(const uint8_t* glyph,const uint8_t* palette) {
    Image img(16,20);
    for(int y=0;y<20;++y) for(int x=0;x<16;++x) {
        const uint8_t index=(glyph[y*8+x/2]>>((x&1)*4))&15;
        const uint8_t* c=palette+index*4; uint8_t* o=img.at(x,y);
        o[0]=c[0];o[1]=c[1];o[2]=c[2];o[3]=uint8_t(std::min(255,c[3]*2));
    }
    return img;
}
inline Image crop(const Image& src,int x0,int y0,int w,int h) {
    Image out(w,h);
    for(int y=0;y<h;++y) for(int x=0;x<w;++x)
        if(x0+x>=0 && x0+x<src.w && y0+y>=0 && y0+y<src.h) std::memcpy(out.at(x,y),src.at(x0+x,y0+y),4);
    return out;
}

// Letters split by shape: a letter's fill (bright, mostly opaque pixels) is one connected region; every other inked
// pixel belongs to the nearest fill, so outlines that touch a neighbour stay with their own letter.
struct Letter { Image img; std::vector<uint8_t> idx; int fill0=0,fill1=0; };   // fill0..fill1: the fill's columns
                                                        // inside img; idx: palette indices (255 = empty) if known
inline bool isFill(const uint8_t* p) { return p[3]>128 && (p[0]+p[1]+p[2])>450; }
inline std::vector<Letter> splitLetters(const Image& word,size_t count,const std::vector<uint8_t>* indices=nullptr) {
    const int W=word.w,H=word.h;
    std::vector<int> label(size_t(W)*H,0); std::vector<size_t> sizes;
    for(int y=0;y<H;++y) for(int x=0;x<W;++x) {
        if(label[size_t(y)*W+x] || !isFill(word.at(x,y))) continue;
        const int id=int(sizes.size())+1; size_t size=0;
        std::vector<int> stack{y*W+x}; label[size_t(y)*W+x]=id;
        while(!stack.empty()) {
            const int c=stack.back(); stack.pop_back(); ++size;
            for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
                const int nx=c%W+dx,ny=c/W+dy;
                if(nx<0||ny<0||nx>=W||ny>=H||label[size_t(ny)*W+nx]||!isFill(word.at(nx,ny))) continue;
                label[size_t(ny)*W+nx]=id; stack.push_back(ny*W+nx);
            }
        }
        sizes.push_back(size);
    }
    if(sizes.size()<count) return {};
    std::vector<int> ids(sizes.size()); for(size_t i=0;i<ids.size();++i) ids[i]=int(i)+1;
    std::sort(ids.begin(),ids.end(),[&](int a,int b){return sizes[a-1]>sizes[b-1];});      // drop specks
    ids.resize(count);
    std::vector<int> left(sizes.size()+1,W),right(sizes.size()+1,-1);
    for(int y=0;y<H;++y) for(int x=0;x<W;++x) if(int l=label[size_t(y)*W+x]) { left[l]=std::min(left[l],x); right[l]=std::max(right[l],x); }
    std::sort(ids.begin(),ids.end(),[&](int a,int b){return left[a]<left[b];});
    std::vector<Letter> out(count);
    std::vector<int> owner(size_t(W)*H,-1);
    for(int y=0;y<H;++y) for(int x=0;x<W;++x) {
        if(word.at(x,y)[3]==0) continue;
        double best=1e30;
        for(size_t k=0;k<count;++k) {
            const int id=ids[k];
            for(int yy=0;yy<H;++yy) for(int xx=left[id];xx<=right[id];++xx) {
                if(label[size_t(yy)*W+xx]!=id) continue;
                const double d=double(xx-x)*(xx-x)+double(yy-y)*(yy-y);
                if(d<best) { best=d; owner[size_t(y)*W+x]=int(k); }
            }
        }
    }
    for(size_t k=0;k<count;++k) {
        int x0=W,x1=-1;
        for(int y=0;y<H;++y) for(int x=0;x<W;++x) if(owner[size_t(y)*W+x]==int(k)) { x0=std::min(x0,x); x1=std::max(x1,x); }
        if(x1<x0) return {};
        Letter& l=out[k]; l.img=Image(x1-x0+1,H);
        if(indices) l.idx.assign(size_t(l.img.w)*H,255);
        for(int y=0;y<H;++y) for(int x=x0;x<=x1;++x) if(owner[size_t(y)*W+x]==int(k)) {
            std::memcpy(l.img.at(x-x0,y),word.at(x,y),4);
            if(indices) l.idx[size_t(y)*l.img.w+x-x0]=(*indices)[size_t(y)*W+x];
        }
        l.fill0=left[ids[k]]-x0; l.fill1=right[ids[k]]-x0;
    }
    return out;
}
// A word from letters (nullptr = space of `space` columns): fills `fillGap` columns apart, as in the game's words.
// With `outIdx`, also the palette indices (letters split with indices): where outlines overlap, the more opaque wins.
inline Image buildWord(const std::vector<const Letter*>& letters,int space,int fillGap,int margin=4,
                       std::vector<uint8_t>* outIdx=nullptr) {
    int H=0; for(auto* l:letters) if(l) H=std::max(H,l->img.h);
    struct Place { const Letter* l; int left; };
    std::vector<Place> placed; int pen=margin,right=0;
    for(auto* l:letters) {
        if(!l) { pen+=space; continue; }
        const int left=pen-l->fill0; placed.push_back({l,left});
        right=std::max(right,left+l->img.w); pen=left+l->fill1+1+fillGap;
    }
    const int shift=placed.empty()?0:std::max(0,-placed.front().left);
    Image out(right+shift+margin,H);
    if(outIdx) outIdx->assign(size_t(out.w)*H,255);
    for(const auto& p:placed) for(int y=0;y<p.l->img.h;++y) for(int x=0;x<p.l->img.w;++x) {
        const uint8_t* s=p.l->img.at(x,y); uint8_t* d=out.at(p.left+shift+x,y);
        if(outIdx && !p.l->idx.empty() && s[3]>d[3]) (*outIdx)[size_t(y)*out.w+p.left+shift+x]=p.l->idx[size_t(y)*p.l->img.w+x];
        const float a=s[3]/255.0f;
        for(int c=0;c<3;++c) d[c]=uint8_t(std::lround(d[c]*(1-a)+s[c]*a));
        d[3]=std::max(d[3],s[3]);
    }
    return out;
}

// Title font: solid letters (alpha >= kCore) and a soft glow whose alpha and colour depend on the distance to the
// nearest solid pixel. The profile is learned from one of the game's titles and regenerated around new words.
inline constexpr uint8_t kCore=230;
struct GlowProfile { std::vector<float> alpha; std::vector<std::array<float,3>> colour; };
inline std::vector<float> coreDistance(const Image& img) {
    std::vector<float> d(size_t(img.w)*img.h,1e9f);
    std::vector<std::pair<int,int>> cores;
    for(int y=0;y<img.h;++y) for(int x=0;x<img.w;++x) if(img.at(x,y)[3]>=kCore) cores.push_back({x,y});
    for(int y=0;y<img.h;++y) for(int x=0;x<img.w;++x)
        for(auto [cx,cy]:cores) d[size_t(y)*img.w+x]=std::min(d[size_t(y)*img.w+x],std::sqrt(float((cx-x)*(cx-x)+(cy-y)*(cy-y))));
    return d;
}
inline GlowProfile learnGlow(const Image& title,int radius=12) {
    GlowProfile p; p.alpha.assign(radius,0); p.colour.assign(radius,{200,192,180});
    const auto d=coreDistance(title);
    for(int k=1;k<radius;++k) {
        double a=0,r=0,g=0,b=0; int n=0;
        for(int y=0;y<title.h;++y) for(int x=0;x<title.w;++x) {
            const float dd=d[size_t(y)*title.w+x]; const uint8_t* px=title.at(x,y);
            if(px[3]>=kCore || dd<=k-0.5f || dd>k+0.5f) continue;
            a+=px[3]; r+=px[0]; g+=px[1]; b+=px[2]; ++n;
        }
        if(n) { p.alpha[k]=float(a/n); p.colour[k]={float(r/n),float(g/n),float(b/n)}; }
    }
    return p;
}
inline Image cores(const Image& title) { Image out=title; for(size_t i=0;i<out.rgba.size();i+=4) if(out.rgba[i+3]<kCore) std::memset(&out.rgba[i],0,4); return out; }
inline Image applyGlow(const Image& coreImg,const GlowProfile& p) {
    Image out=coreImg; const auto d=coreDistance(coreImg); const int R=int(p.alpha.size());
    for(int y=0;y<out.h;++y) for(int x=0;x<out.w;++x) {
        uint8_t* px=out.at(x,y); if(px[3]>=kCore) continue;
        const float dd=d[size_t(y)*out.w+x]; if(dd>=R-1) { std::memset(px,0,4); continue; }
        const int k0=int(dd); const float f=dd-k0; const int k1=std::min(k0+1,R-1);
        px[3]=uint8_t(std::lround(p.alpha[k0]*(1-f)+p.alpha[k1]*f));
        const auto& c=p.colour[std::clamp(int(std::lround(dd)),0,R-1)];
        px[0]=uint8_t(c[0]);px[1]=uint8_t(c[1]);px[2]=uint8_t(c[2]);
    }
    return out;
}
}
