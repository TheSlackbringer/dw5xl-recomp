#!/usr/bin/env python3
# Decode a PSMCT32 region from a PS2X_VRAM_DUMP file (raw 4 MiB GS local memory) to PNG.
# usage: gs_vram_ct32.py vram.bin out.png <base_block> <buffer_width_in_64px_units> <width> <height>
# e.g. display buffer FBP 0: "0 8 512 224"; DW5XL title offscreen FBP 56 = block 1792: "1792 8 512 448".
# Z32 buffers are stored here in CT32 layout too (the CPU GS backend uses psm 0 swizzle for Z32).
import sys, numpy as np
from PIL import Image
vram=np.fromfile(sys.argv[1],dtype='<u4')
BT=[[0,1,4,5,16,17,20,21],[2,3,6,7,18,19,22,23],[8,9,12,13,24,25,28,29],[10,11,14,15,26,27,30,31]]
CT=[[0,1,4,5,8,9,12,13],[2,3,6,7,10,11,14,15],[16,17,20,21,24,25,28,29],[18,19,22,23,26,27,30,31],
    [32,33,36,37,40,41,44,45],[34,35,38,39,42,43,46,47],[48,49,52,53,56,57,60,61],[50,51,54,55,58,59,62,63]]
def dec(bp,bw,w,h):
    img=np.zeros((h,w,4),np.uint8)
    for y in range(h):
        for x in range(w):
            page=(y//32)*bw+(x//64)
            block=bp+page*32+BT[(y%32)//8][(x%64)//8]
            wd=vram[(block*64+CT[y%8][x%8])%len(vram)]
            img[y,x]=[wd&255,(wd>>8)&255,(wd>>16)&255,255]
    return img
bp,bw,w,h=map(int,sys.argv[3:7])
Image.fromarray(dec(bp,bw,w,h)).save(sys.argv[2])
