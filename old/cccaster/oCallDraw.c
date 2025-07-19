#include <stdio.h>
/**
Original file that CallDraw.s is compiled from via gcc -S
not actually included in compilation, requires hand editing to insert positional arguments into registers for external function calls
drawtext:   textWidth->EAX
            xAddr->ECX
drawSprite: spriteWidth->EDX
drawRect:   dxObj->EAX
*/


extern "C" {
    void (*drawtext) (int, char*, int, int, int, void*, int, int, int) = (void(*)(int, char*, int, int, int, void*, int, int, int)) 0x41d340;
    void (*drawsprite) (int, int, int, int, int, int, int, int, int, int, int, int) = (void(*)(int, int, int, int, int, int, int, int, int, int, int, int)) 0x415580;
    void (*drawrect) (int, int, int, int, int, int, int, int, int) = (void(*)(int, int, int, int, int, int, int, int, int)) 0x415450;
    int (*createTexFromFileInMemory) (int, char*, int, int) = (int(*)(int, char*, int, int)) 0x4bd2d0;

    int CallDrawText ( int width, int height, int xAddr, int yAddr, char* text, int textAlpha, int textShade, int layer, void* addr, int spacing, int unk, char* out ) {
        (*drawtext)( yAddr, text, textShade, textAlpha, layer, addr, height, spacing, unk );
        return 1;
    }

    int CallDrawSprite ( int spriteWidth, int dxdevice, int texAddr, int screenXAddr, int screenYAddr, int spriteHeight, int texXAddr, int texYAddr, int texXSize, int texYSize, int flags, int unk, int layer ) {
        (*drawsprite)( dxdevice, texAddr, screenXAddr, screenYAddr, spriteHeight, texXAddr, texYAddr, texXSize, texYSize, flags, unk, layer );
        return 1;
    }

    int CallDrawRect ( int screenXAddr, int screenYAddr, int width, int height, int A, int B, int C, int D, int layer ) {
        (*drawrect)( screenXAddr, screenYAddr, width, height, A, B, C, D, layer );
        return 1;
    }

    int loadTextureFromMemory( char* imgbuf1, int img1size, char* imgbuf2, int img2size, int param4 ) {
        return (*createTexFromFileInMemory)( img1size, imgbuf2, img2size, param4 );
    }
}
