#!/bin/bash
# Offline checks of the W2VR engine camera against the game's own code (no game run needed).
#   W2_UNPACKED=/path/to/witcher2_unpacked.exe tests/run_tests.sh
# needs: python3 with numpy, unicorn, capstone, pefile; g++; the DLL built by scripts/build.sh. The exe must be YOUR
# copy, decrypted locally (the Steam exe's code is encrypted by SteamStub 2.1; Steamless decrypts a copy).
set -e
cd "$(dirname "$0")"
mkdir -p build
g++ -O2 -shared -fPIC -std=c++17 -I../src -I../third_party/xrbridge/include compose_shim.cpp -o build/libw2compose.so
g++ -O2 -shared -fPIC -std=c++17 -I../src -I../third_party/xrbridge/include detect_shim.cpp ../src/stereo.cpp -o build/libw2detect.so
python3 check_camera.py 400
python3 check_detect.py
python3 check_dll_hook.py
python3 check_sametick.py
python3 check_letterbox.py
python3 check_dof.py
python3 check_fullstereo.py
python3 check_compat.py
python3 check_dither.py
