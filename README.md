# TrafficMonitor Traffic Stats Plugin

A TrafficMonitor plugin for monitoring **per-process network traffic** in real time and in accumulated history.

## Features

- Real-time upload and download statistics for each process
- Historical traffic totals by **day**, **month**, and **year**
- Detailed traffic window with process icons
- English / Chinese interface support
- Preference persistence across restarts

## Requirements

- TrafficMonitor with plugin API version `7`
- Windows x64
- Visual Studio 2022 or Build Tools for Visual Studio 2022
- Build target: `Release | x64`

## Build

Open `ProcessTrafficPlugin.sln` in Visual Studio 2022 or Build Tools for Visual Studio 2022, then build with:

- **Configuration:** `Release`
- **Platform:** `x64`

You can also build from the command line with MSBuild:

```powershell
msbuild "ProcessTrafficPlugin.sln" /p:Configuration=Release /p:Platform=x64
```

## Install

1. Build the project successfully.
2. Copy `ProcessTrafficPlugin.dll` into the `plugins`.
3. Start TrafficMonitor **as Administrator**.
4. Open the plugin manager and confirm that the plugin is loaded.

## Download

If you do not want to build the plugin yourself, you can download the prebuilt package from the **Releases** page.

## Notes

- This plugin relies on **ETW (Event Tracing for Windows)** to collect per-process traffic data.
- In many cases, TrafficMonitor must be started with **administrator privileges**; otherwise traffic collection may fail and you may see errors such as `StartTrace failed`.
