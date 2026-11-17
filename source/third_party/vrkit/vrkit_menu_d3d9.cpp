#include "vrkit_menu_d3d9.h"
#include "vrkit_options.h"
#include <windows.h>
#include "imgui.h"
#include "imgui_impl_dx9.h"

namespace vrkit { namespace menu9 {

namespace {
IDirect3DDevice9* g_dev = nullptr;
IDirect3DTexture9* g_tex = nullptr;
bool g_ok = false, g_backend = false;

bool EnsureTexture() {
    if (g_tex) return true;
    const auto& s = menu::core::Get();
    HRESULT hr = g_dev->CreateTexture(s.texW, s.texH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_tex, nullptr);
    if (FAILED(hr)) { Log("vrkit: menu texture failed %08lx", hr); g_tex = nullptr; return false; }
    return true;
}

void Draw() {
    if (!EnsureTexture()) return;
    const auto& s = menu::core::Get();
    IDirect3DSurface9 *rt0 = nullptr, *ds = nullptr, *surf = nullptr;
    g_dev->GetRenderTarget(0, &rt0);
    g_dev->GetDepthStencilSurface(&ds);
    D3DVIEWPORT9 vpOld; g_dev->GetViewport(&vpOld);
    g_tex->GetSurfaceLevel(0, &surf);
    g_dev->SetRenderTarget(0, surf);
    g_dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = { 0, 0, s.texW, s.texH, 0.f, 1.f };
    g_dev->SetViewport(&vp);
    g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.f, 0);
    ImGui_ImplDX9_NewFrame();
    menu::core::BuildFrame();
    if (g_dev->BeginScene() >= 0) { ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData()); g_dev->EndScene(); }
    g_dev->SetRenderTarget(0, rt0);
    g_dev->SetDepthStencilSurface(ds);
    g_dev->SetViewport(&vpOld);
    if (surf) surf->Release();
    if (rt0) rt0->Release();
    if (ds) ds->Release();
}
} // namespace

bool Init(IDirect3DDevice9* dev, const menu::Settings& s) {
    if (g_ok) return true;
    g_dev = dev;
    if (!menu::core::InitImGui(s)) return false;
    if (!ImGui_ImplDX9_Init(dev)) { Log("vrkit: ImGui D3D9 init failed"); return false; }
    g_backend = true;
    g_ok = true;
    Log("vrkit: D3D9 menu ready (%zu options on %zu pages)", Options::Get().All().size(), Options::Get().Pages().size());
    return true;
}

void Shutdown() {
    if (g_backend) ImGui_ImplDX9_Shutdown();
    menu::core::ShutdownImGui();
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    g_ok = g_backend = false;
}

void OnLostDevice() {
    if (g_backend) ImGui_ImplDX9_InvalidateDeviceObjects();
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
}

bool Update() {
    if (!g_ok || !menu::core::UpdateInput()) return false;
    Draw();
    return true;
}
bool IsOpen() { return menu::core::IsOpen(); }
void SetOpen(bool open) { menu::core::SetOpen(open); }
IDirect3DTexture9* Texture() { return g_tex; }
UINT Width() { return menu::core::Get().texW; }
UINT Height() { return menu::core::Get().texH; }

} } // namespace vrkit::menu9
