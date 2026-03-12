# TrafficMonitor Traffic Stats Plugin

[简体中文](./README.md) | English

A TrafficMonitor plugin for monitoring **per-process network traffic** in real time and in accumulated history.

This project is a third-party plugin for [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor).  
It is not part of the official TrafficMonitor project.

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

## Installation

1. Build the project successfully.
2. Copy `ProcessTrafficPlugin.dll` into the `plugins` directory.
3. Start TrafficMonitor **as Administrator**.
4. Open the plugin manager and confirm that the plugin is loaded.

## Download

If you do not want to build the plugin yourself, you can download the prebuilt package from the [Releases](../../releases) page.

## Notes

- This plugin relies on **ETW (Event Tracing for Windows)** to collect per-process traffic data.
- In many cases, TrafficMonitor must be started with **administrator privileges**; otherwise traffic collection may fail and you may see errors such as `StartTrace failed`.

## Build

Open `ProcessTrafficPlugin.sln` in Visual Studio 2022 or Build Tools for Visual Studio 2022, then build with:

- **Configuration:** `Release`
- **Platform:** `x64`

You can also build from the command line with MSBuild:

```powershell
msbuild "ProcessTrafficPlugin.sln" /p:Configuration=Release /p:Platform=x64
```

# Open the main interface

## Right-click the program and open Plugin Management

<img width="587" height="533" alt="image" src="https://github.com/user-attachments/assets/32f020d1-56d8-4d96-8db1-20829a1b6243" />


## Right-click the plugin again to open Traffic Details

<img width="710" height="462" alt="image" src="https://github.com/user-attachments/assets/e9ece008-b293-482a-adcd-f8b49aafc6fc" />


<img width="897" height="620" alt="image" src="https://github.com/user-attachments/assets/ed9b9f6a-befa-4301-acb7-1157e272b63e" />
<img width="920" height="620" alt="image" src="https://github.com/user-attachments/assets/ab1d1180-ad20-4882-964d-30e3774cda1f" />
