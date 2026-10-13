#include "../../app/src/ipu_idec.h"
#include <cstdio>
int main()
{
    const std::string error = ipu::selfTest();
    std::printf("%s\n", error.empty() ? "VLC tables OK" : error.c_str());
    return error.empty() ? 0 : 1;
}
