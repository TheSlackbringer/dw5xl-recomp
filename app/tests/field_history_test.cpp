#include "runtime/gs/field_history.h"
#include <stdexcept>
#include <iostream>
using Mode = ReusableFieldWeaver::Mode;
void require(bool value) { if (!value) throw std::runtime_error("field history assertion"); }
std::vector<uint8_t> rows(std::initializer_list<uint8_t> values) {
    std::vector<uint8_t> pixels;
    for (auto value : values) pixels.insert(pixels.end(), {value,value,value,255,0,0,0,0});
    return pixels;
}
void check(const std::vector<uint8_t>& pixels, std::initializer_list<uint8_t> values) {
    size_t y=0; for (auto value: values) require(pixels.at(y++*8)==value);
}
int main() {
    ReusableFieldWeaver w;
    uint32_t h=4;
    auto p=rows({10,20,30,40});
    require(w.present(p,1,h,8,10,false,Mode::FullHeightField)); check(p,{10,10,30,30});
    h=4;p=rows({10,20,30,40});
    require(w.present(p,1,h,8,11,true,Mode::FullHeightField)); check(p,{10,20,30,40});
    h=4;p=rows({10,20,30,40});
    require(w.present(p,1,h,8,12,false,Mode::FullHeightField)); check(p,{10,20,30,40});
    // Repeating the same tick retains the opposite field, not a stale current row.
    h=4;p=rows({10,20,30,40});
    require(w.present(p,1,h,8,12,false,Mode::FullHeightField)); check(p,{10,20,30,40});
    // FRAME fields update in-place; no framebuffer change is needed.
    h=2;p=rows({10,30});
    require(w.present(p,1,h,8,20,false,Mode::HalfHeightFrame)); require(h==4); check(p,{10,10,30,30});
    h=2;p=rows({20,40});
    require(w.present(p,1,h,8,21,true,Mode::HalfHeightFrame)); check(p,{10,20,30,40});
    // A skipped field never weaves stale opposite rows.
    h=2;p=rows({50,70});
    require(w.present(p,1,h,8,24,false,Mode::HalfHeightFrame)); check(p,{50,50,70,70});
    // Consecutive timestamps with identical parity also invalidate history.
    h=2;p=rows({60,80});
    require(w.present(p,1,h,8,25,false,Mode::HalfHeightFrame)); check(p,{60,60,80,80});
    // Geometry changes and mode changes reset, even at an adjacent tick.
    h=1;p=rows({90});
    require(w.present(p,1,h,8,26,true,Mode::HalfHeightFrame)); check(p,{90,90});
    h=4;p=rows({1,2,3,4});
    require(w.present(p,1,h,8,27,true,Mode::FullHeightField)); check(p,{2,2,4,4});
    // Progressive output is untouched and breaks field continuity.
    h=4;p=rows({1,2,3,4});
    require(w.present(p,1,h,8,28,false,Mode::Progressive)); check(p,{1,2,3,4});
    h=4;p=rows({1,2,3,4});
    require(w.present(p,1,h,8,29,true,Mode::FullHeightField)); check(p,{2,2,4,4});
    // Odd output height has a bounded last source row.
    h=3;p=rows({1,2,3});
    require(w.present(p,1,h,8,30,false,Mode::FullHeightField)); check(p,{1,1,3});
    // Regressed boot clock resets history.
    h=3;p=rows({5,6,7});
    require(w.present(p,1,h,8,1,true,Mode::FullHeightField)); check(p,{6,6,7});
    // Independent instances cannot inherit the first instance's opposite rows.
    ReusableFieldWeaver other;
    h=3;p=rows({8,9,10});
    require(other.present(p,1,h,8,2,false,Mode::FullHeightField)); check(p,{8,8,10});
    h=4;p=rows({1});
    require(!w.present(p,1,h,8,3,true,Mode::FullHeightField));
    // High-contrast comb teeth in fast motion are adaptively blended towards spatial neighbors.
    ReusableFieldWeaver combWeaver;
    h=2;p=rows({200,200});
    require(combWeaver.present(p,1,h,8,40,false,Mode::HalfHeightFrame));
    h=2;p=rows({0,0});
    require(combWeaver.present(p,1,h,8,41,true,Mode::HalfHeightFrame));
    require(p.at(2*8) < 50);
    // Stationary fine detail must not be mistaken for combing merely because
    // it falls between dark rows of the opposite parity. The temporal guard
    // deliberately waits for both same-parity histories before preserving it.
    for (Mode mode : {Mode::FullHeightField, Mode::HalfHeightFrame}) {
        for (unsigned brightRow : {2u,3u}) {
            ReusableFieldWeaver detail;
            for (uint64_t tick=40;tick<=48;++tick) {
                std::vector<uint8_t> image(6*8,0);
                for(unsigned y=0;y<6;++y) {
                    for(unsigned c=0;c<3;++c)image[y*8+c]=y==brightRow?255:0;
                    image[y*8+3]=255;
                }
                const bool odd=(tick&1u)!=0;
                if(mode==Mode::HalfHeightFrame) {
                    std::vector<uint8_t> field;
                    for(unsigned y=odd?1:0;y<6;y+=2)
                        field.insert(field.end(),image.begin()+y*8,image.begin()+(y+1)*8);
                    image=std::move(field);
                    h=3;
                } else h=6;
                require(detail.present(image,1,h,8,tick,odd,mode));
                if(tick>=43) {
                    require(h==6);
                    for(unsigned y=0;y<6;++y)
                        require(image[y*8]==(y==brightRow?255:0));
                }
                // Duplicate presentation must retain same-parity history.
                if(tick==44) {
                    auto repeat=rows({0,0,brightRow==2?uint8_t(255):uint8_t(0),
                                      brightRow==3?uint8_t(255):uint8_t(0),0,0});
                    if(mode==Mode::HalfHeightFrame) {
                        auto full=repeat;repeat.clear();
                        for(unsigned y=0;y<6;y+=2)
                            repeat.insert(repeat.end(),full.begin()+y*8,full.begin()+(y+1)*8);
                        h=3;
                    } else h=6;
                    require(detail.present(repeat,1,h,8,tick,false,mode));
                    require(repeat[brightRow*8]==255);
                }
            }
        }
    }
    // Actual temporal change still enables the existing spatial comb filter.
    ReusableFieldWeaver changedDetail;
    h=6;p=rows({0,0,255,0,0,0}); require(changedDetail.present(p,1,h,8,50,false,Mode::FullHeightField));
    h=6;p=rows({0,0,255,0,0,0}); require(changedDetail.present(p,1,h,8,51,true,Mode::FullHeightField));
    h=6;p=rows({0,0,255,0,0,0}); require(changedDetail.present(p,1,h,8,52,false,Mode::FullHeightField));
    h=6;p=rows({0,10,255,10,0,0}); require(changedDetail.present(p,1,h,8,53,true,Mode::FullHeightField));
    require(p[2*8]==10); // neighbours moved: do not preserve old white stroke
    // Dropped fields (gap 2: same parity again; gap 3: two missing) keep the
    // history, so stationary detail stays stable instead of re-warming.
    for (uint64_t dropped : {42u, 43u}) {
        ReusableFieldWeaver drops;
        for (uint64_t tick=30;tick<=50;++tick) {
            if (tick==dropped || (dropped==43u && tick==42u)) continue;
            const bool odd=(tick&1u)!=0;
            h=3;p=odd?rows({0,0,0}):rows({0,255,0}); // white stroke on even row 2
            require(drops.present(p,1,h,8,tick,odd,Mode::HalfHeightFrame));
            if (tick>=34) { require(h==6); check(p,{0,0,255,0,0,0}); }
        }
    }
    // A stale opposite field is never woven where the picture moved.
    ReusableFieldWeaver staleMotion;
    h=2;p=rows({0,0}); require(staleMotion.present(p,1,h,8,60,false,Mode::HalfHeightFrame));
    h=2;p=rows({200,200}); require(staleMotion.present(p,1,h,8,61,true,Mode::HalfHeightFrame));
    h=2;p=rows({0,0}); require(staleMotion.present(p,1,h,8,62,false,Mode::HalfHeightFrame));
    h=2;p=rows({200,200}); require(staleMotion.present(p,1,h,8,63,true,Mode::HalfHeightFrame));
    h=2;p=rows({90,90}); require(staleMotion.present(p,1,h,8,66,false,Mode::HalfHeightFrame)); // 64,65 dropped
    check(p,{90,90,90,90});
    // Faint motion combing (here 15 levels) is blended too, not left as stripes.
    ReusableFieldWeaver faintComb;
    h=2;p=rows({100,100}); require(faintComb.present(p,1,h,8,70,false,Mode::HalfHeightFrame));
    h=2;p=rows({115,115}); require(faintComb.present(p,1,h,8,71,true,Mode::HalfHeightFrame));
    require(p.at(2*8) >= 110); // opposite-field row pulled toward its moving neighbours
    std::cout << "Field history tests passed\n";
}
