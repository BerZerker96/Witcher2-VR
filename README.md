<img width="2047" height="1152" alt="1794818480005" src="https://github.com/user-attachments/assets/25adb821-f23c-4fce-aa6f-39c5269b2b8c" />
<div align="center">

# 🐺 The Witcher 2 VR (W2VR) Steam\Gog 

**VR for The Witcher 2: Assassins of Kings – Enhanced Edition on PC**

Stereo 3D · head and position tracking · first person · in-headset settings panel · any OpenXR headset

![version](https://img.shields.io/badge/version-1.0.8-c0392b) ![game](https://img.shields.io/badge/game-The%20Witcher%202%20EE-222) ![xr](https://img.shields.io/badge/OpenXR-SteamVR%20%7C%20Pimax%20%7C%20Quest%20%7C%20VDXR%20%7C%20WMR-555)

</div>

<img width="4795" height="1601" alt="w2" src="https://github.com/user-attachments/assets/2d8bf8c3-e425-490c-9f73-bbe3b0e28825" />



---

## 📦 Install

1. Copy everything inside the `mod` folder into `The Witcher 2\bin` (next to `witcher2.exe`).
2. **Skip the launcher:** double-click `W2VR_SkipLauncher_Install.bat` there once. From now on, **Play** in Steam or GOG goes straight into the game and VR, with no launcher window. Hold **SHIFT** while pressing Play to get the original launcher (Configurator, DLC). `W2VR_SkipLauncher_Uninstall.bat` puts it back.
3. **Game settings (once): open the **Configurator** and set:
   - **AllowDecals:** **Medium** or **Disabled**. Decals are blood and hit splatter. On **High** they slow fights down in VR.
   - **TextureMemoryBudget:** **VeryHighBudget**. The mod sets this for you too.
4. Start your VR software (Pimax Play, SteamVR, Virtual Desktop, Oculus …), then start the game. VR starts by itself.

<img width="397" height="662" alt="2026-11-17 03_16_21-Configurator" src="https://github.com/user-attachments/assets/56fa622e-e369-4c15-903a-c526f7f4f97c" />


**Game hangs on the medallion screen?** (only on CPUs with 32+ threads) Start the game with `W2VR_Launch.bat` instead. It launches straight into the game and VR, limited to 16 threads.

To uninstall, delete the copied files and the `W2VR` folder. Your original settings are kept in `Documents\Witcher 2\Config\User.ini.w2vr_backup`.

---

## 🎮 Controls

| Keyboard | Gamepad | Action |
|---|---|---|
| `Insert` | hold `L3 + R3` | Open / close the VR panel |
| `Home` | – | Recentre the view |
| `End` | – | First person on / off |
| `F7` | – | HUD: floating panel / fitted in view / full size |
| `F8` | – | Switch stereo / head-tracked mono / flat screen |
| `Delete` | – | Screenshot |

---

## 🥽 Your headset is set up automatically

Each time the game starts, the mod asks your headset for its field of view and recommended resolution, and sets the game's picture to match. The picture fills your view at the sharpness your VR software asks for, with no black bars and nothing cut off at the edges.

- Want it sharper? Raise **Display > Render resolution** in the VR panel (for example 1.2). Want more fps? Lower it.
- Changed your VR software's resolution? The mod follows it on the next start.

---

## 👀 Stereo methods

Pick one in **View > Stereo method**. You can switch while playing.

| Method | What it does |
|---|---|
| **Auto** | The default. Same-tick while your headset runs at full rate, Full stereo while headset smoothing is on. |
| **Full stereo** | Both eyes are drawn from every game tick. Perfect depth in motion, but the scene is drawn twice per tick, so fps is about half (heavy in combat). Best with headset smoothing on **Always**. |
| **Same-tick AER** | One eye per frame (left, right, left …). Game time pauses for the second eye, so both eyes show the same moment. |

**Headset smoothing** (Pimax Smart Smoothing, SteamVR Motion Smoothing, ASW) is in **Engine > Headset smoothing**:

- **Only when fps drops** (default): every frame is a real frame at your headset's full rate.
- **Always**: the game runs at half rate and your headset fills in frames. Smoothing must also be on in your VR software.
- **Off**: the game never follows the headset's smoothing.

---

## ✨ What you get

- **Real 3D with head tracking.** Your head moves the game's own camera, so lighting, shadows and culling are right for each eye.
- **Sharp picture.** Full-resolution textures, 16× anisotropic filtering and the game's sharpen. Depth of field, motion blur, vignette and the game's soft anti-aliasing are off.
- **First person** (`End`), with adjustable position.
- **No black edges:** edge fill stretches the picture's edge outward, smooth, a few degrees beyond your view. When fps dips and the headset re-projects an older frame, you see that instead of black.
- **Decoupled pitch:** the camera stays level and you look up and down with your head (less motion sickness).
- **HUD and menus on a floating panel** that follows your head, with adjustable size, distance and transparency.
- **Gamepad works** in VR, even when the game window isn't focused.
- **Smooth frame pacing:** a frame cap at your headset's refresh rate.
- **Menus, loading screens and videos** on a flat virtual screen.

---

## 🧰 VR panel (`Insert` / `L3 + R3`)

| Page | What's there |
|---|---|
| **View** | VR mode, stereo method, world scale, head and position tracking, edge fill (smooth, extra margin), recentre |
| **Camera** | Decoupled pitch, camera offsets, first person (distance, height, offsets) |
| **HUD** | HUD mode, panel distance / width / height, follow head, HUD / UI transparency (60 % by default) |
| **Display** | Render resolution, aspect ratio, screenshots |
| **Engine** | Frame limit, headset smoothing, texture filtering, sharpen, depth of field, motion blur, vignette, SSAO |
| **Screen** | Flat screen size and distance, panel size and opacity |
| **Tests** | Diagnosis tools: show one G-buffer layer, hide decals, texture budget (not saved) |

Settings are saved in `w2vr_config.txt` next to the game exe. Delete that file to get the defaults back.

---

## ❓ Problems?

- **No picture in the headset:** start your VR software before the game, and make sure it is the active OpenXR runtime.
- **Slow-downs in fights:** set **AllowDecals** to **Medium** or **Disabled** in the Configurator (see Install).
- **Low fps or stutter:** use **View > Stereo method > Auto** (Full stereo halves fps), lower **Display > Render resolution** (0.9 helps a lot), or set **Engine > Frame limit** to 60.
- **OpenXR layers** (OpenXR Toolkit and others) work as with any OpenXR game. The log lists the ones that are enabled.
- **Anything else:** the mod writes `w2vr.log` in `The Witcher 2\bin\W2VR`. Send it with your report. Every 10 seconds it records fps, frame times and what limits the frame rate.

---

## 🏗️ Building

The `source` folder holds the full source. Build with MinGW-w64: `source/scripts/build.sh`. Technical details and the full history are in `NOTES.md`.

---

## 🙏 Credits

| Who | What |
|---|---|
| **mark-mon** | VR framework these mods are built on: XrBridge OpenXR host, vrkit VR panel, proxy loader |
| **TefMeister** | witcher-2-vr research: SteamStub, camera axes, User.ini keys, render thread |
| **The Witcher 2 6DOF head-tracking mod** | The camera hook site |
| **praydog / UEVR** | The synchronized-sequential AER idea behind same-tick |
| **crosire / ReShade** | The 2016 fix that showed why metal and eyes rendered in the wrong colours |
| **atom0s** | Steamless (the method used locally to study the game's code; not included) |
| **Tsuda Kageyu** | MinHook (BSD 2-Clause) |
| **Omar Cornut** | Dear ImGui (MIT) |
| **Sean Barrett** | stb_image_write (MIT / public domain) |
| **Khronos Group** | OpenXR (Apache 2.0) |

<sub>The Witcher is a trademark of CD PROJEKT S.A. This is an unofficial fan mod with no game files included. You need your own copy of the game.</sub>
