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
