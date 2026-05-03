# Repository Guidelines

## Project Structure & Module Organization
`ezorsia.sln` is the top-level Visual Studio solution. The main DLL project lives in `ezorsia/`, with gameplay patches and hooks split across focused files such as `ijl15.cpp`, `dllmain.cpp`, `Memory.cpp`, `BossHP.cpp`, and `HpMpAlert.cpp`. Shared MapleStory container and pointer wrappers are under `ezorsia/MapleClientCollectionTypes/`. Bundled third-party binaries live in `detours/`. Runtime settings are stored in `ezorsia/config.ini`.

## Build, Test, and Development Commands
Build this project with Visual Studio 2019 and the v142 toolset; the README recommends `Release|Win32`.

```powershell
msbuild ezorsia.sln /p:Configuration=Release /p:Platform=Win32
```

This produces `out/Release/ijl15.dll`. For local debugging, use `Debug|Win32` from Visual Studio. After building, deploy `ijl15.dll` and `ezorsia/config.ini` into the target client directory.

## Coding Style & Naming Conventions
Follow the existing C++ style in `ezorsia/`: tabs for indentation, braces on the same line, and Windows/MSVC-friendly headers via `stdafx.h`. Keep filenames in PascalCase or existing legacy names (`BossHP.cpp`, `FixIme.h`, `ijl15.cpp`) and prefer matching `.h`/`.cpp` pairs for new modules. Reuse existing naming patterns: `g_`-style globals if already present nearby, PascalCase for types, and descriptive hook/helper names.

## Testing Guidelines
There is no automated test suite in this repository. Validate changes by building `Release|Win32`, launching the client with the patched DLL, and checking the affected flow in-game. For config-driven features, add a toggle in `ezorsia/config.ini`, keep non-resolution features off by default for public-facing changes, and document the manual verification steps in the PR.

## Commit & Pull Request Guidelines
Recent history favors short conventional-style subjects such as `feat(core): ...`, `feat(network): ...`, and `fix(core): ...`. Keep commits focused and scoped by subsystem. Pull requests should describe the gameplay impact, note any memory addresses or hooks touched, link related issues, and include screenshots or video for UI/resolution changes. If a change is optional, state the new `config.ini` key and its default value.

