/* build-time check of the COM vtable slots d3d9hooks.cpp uses */
#define COBJMACROS
#include <stddef.h>
#include <d3d9.h>
#define SLOT(T, m, n) _Static_assert(offsetof(T##Vtbl, m) / sizeof(void*) == n, #T "::" #m);
SLOT(IDirect3DDevice9, Reset, 16) SLOT(IDirect3DDevice9, Present, 17) SLOT(IDirect3DDevice9, CreateTexture, 23)
SLOT(IDirect3DDevice9, CreateVolumeTexture, 24) SLOT(IDirect3DDevice9, CreateCubeTexture, 25)
SLOT(IDirect3DDevice9, CreateVertexBuffer, 26) SLOT(IDirect3DDevice9, CreateIndexBuffer, 27)
SLOT(IDirect3DDevice9, SetRenderTarget, 37) SLOT(IDirect3DDevice9, SetViewport, 47) SLOT(IDirect3DDevice9, StretchRect, 34) SLOT(IDirect3DDevice9, DrawPrimitive, 81)
SLOT(IDirect3DDevice9, DrawIndexedPrimitive, 82) SLOT(IDirect3DDevice9, DrawPrimitiveUP, 83)
SLOT(IDirect3DDevice9, DrawIndexedPrimitiveUP, 84) SLOT(IDirect3DDevice9, CreateVertexShader, 91)
SLOT(IDirect3DDevice9, SetVertexShader, 92) SLOT(IDirect3DDevice9, SetVertexShaderConstantF, 94)
SLOT(IDirect3DDevice9Ex, Reset, 16) SLOT(IDirect3DDevice9Ex, Present, 17)
SLOT(IDirect3D9Ex, CreateDevice, 16) SLOT(IDirect3DSwapChain9, Present, 3) SLOT(IDirect3DTexture9, LockRect, 19) SLOT(IDirect3DTexture9, UnlockRect, 20)
SLOT(IDirect3DTexture9, AddDirtyRect, 21) SLOT(IDirect3DTexture9, Release, 2)
SLOT(IDirect3DDevice9, SetRenderState, 57) SLOT(IDirect3DDevice9, SetScissorRect, 75)
