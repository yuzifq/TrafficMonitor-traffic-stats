# ProcessTrafficPlugin

TrafficMonitor plugin for viewing per-process network traffic in real time and in accumulated history.

## Features

- Real-time per-process upload and download statistics
- Historical traffic totals by day, month, and year
- Detail window with process icons
- English / Chinese UI switch
- Saved preferences after restart

## Requirements

- TrafficMonitor with plugin API version `7`
- Windows x64
- `Release | x64` build output

## Build

Open `ProcessTrafficPlugin.sln` in Visual Studio 2022 Build Tools / Visual Studio and build:

- Configuration: `Release`
- Platform: `x64`

Or use MSBuild:

```powershell
"C:\D\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" "ProcessTrafficPlugin.sln" /p:Configuration=Release /p:Platform=x64
```

## Install

1. Build the project.
2. Copy `ProcessTrafficPlugin.dll` into the `plugins` folder next to `TrafficMonitor.exe`.
3. Start TrafficMonitor.
4. Open plugin management and make sure the plugin is loaded.

## Files to publish

For source release, keep:

- `ProcessTrafficPlugin.sln`
- `ProcessTrafficPlugin/`
- `README.md`
- `.gitignore`

For end users, package only:

- `ProcessTrafficPlugin.dll`
- a short install note

Do **not** publish:

- `*.pdb`
- `*.obj`
- `*.lib`
- `*.exp`
- `build/`
- `.vs/`

## Privacy note

Do not upload debug symbols (`.pdb`) or local build folders. Those files can expose local machine paths and environment details.

## License

Add your preferred license before publishing publicly.
