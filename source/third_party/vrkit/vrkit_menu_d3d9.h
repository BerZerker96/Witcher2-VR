// vrkit - in-game settings menu, D3D9 backend (same controls and options as the D3D11 one, see vrkit_menu.h).
// Draws the menu with Dear ImGui (imgui_impl_dx9) into an A8R8G8B8 render-target texture, premultiplied alpha.
// Call Update() once per frame on the device's thread (e.g. in the Present hook). Before IDirect3DDevice9::Reset
// call OnLostDevice(); the texture is recreated on the next Update().
#pragma once
#include <d3d9.h>
#include "vrkit_menu_core.h"

namespace vrkit { namespace menu9 {

bool Init(IDirect3DDevice9* dev, const menu::Settings& s);
void Shutdown();
bool Update();               // poll input, redraw while open; true while open
bool IsOpen();
void SetOpen(bool open);
void OnLostDevice();
IDirect3DTexture9* Texture();   // nullptr while not created
UINT Width();
UINT Height();

} } // namespace vrkit::menu9
