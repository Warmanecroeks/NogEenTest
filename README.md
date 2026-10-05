# MomentumFishing

Standalone Windows x64 fishing-minigame macro for MomentumRP/FiveM.

## Controls

- F6: Start/Stop
- F7: Emergency stop
- F8: Calibration: put the mouse cursor over the center of the fishing circle and press F8.

The program reads a small screen region and classifies the colored center of the fishing circle:
- Green -> hold LMB
- Orange/Red -> release LMB

The program automatically releases LMB if GTA V/FiveM is no longer the foreground window.

## Build

### GitHub Actions (recommended)
1. Create a new GitHub repository.
2. Upload all files from this folder.
3. Go to Actions -> Build MomentumFishing.exe.
4. Run workflow.
5. After it finishes, open the workflow run and download the artifact `MomentumFishing-Windows-x64`.

### Local Windows build
Install Visual Studio 2022 Build Tools with Desktop C++ tools, then:
cmake -S . -B build
cmake --build build --config Release

The executable will be:
build\Release\MomentumFishing.exe

## Notes

The default detection point is tuned from the supplied 1280x720 recording. Use F8 for calibration on other resolutions/UI layouts.
