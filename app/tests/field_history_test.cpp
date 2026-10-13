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
    std::cout << "Field history tests passed\n";
}
