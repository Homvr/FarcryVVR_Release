#pragma once
#include <d3d11.h>

namespace fc5::menublit
{
    // A Bink video frame of the menus (intro, title, main menu background, loading) drawn again over the whole current
    // render target from its planes (yuv: luma, Cr, Cb), as the game's YUV shaders convert them. The game lays these out
    // for the tall headset image (the intro stretched, the main menu's background clipped to its top-left 16:9 corner).
    // fit: the whole frame as large as it fits, dimmed blurred bands around it; otherwise cover (aspect kept, sides cut).
    // game_cb, mul, add: the game draw's pixel constants and the registers of its ColorMultiplier / ColorAdd (the fade).
    // Called right after the game's draw, before the menu text.
    bool video(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* const yuv[3], ID3D11Buffer* game_cb, int mul, int add, bool fit);
}
