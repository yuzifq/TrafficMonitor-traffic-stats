# TrafficMonitor Traffic Stats Plugin / TrafficMonitor 流量统计插件

A TrafficMonitor plugin for monitoring **per-process network traffic** in real time and in accumulated history.  
一个用于 TrafficMonitor 的插件，可实时监控**每个进程的网络流量**，并统计累计历史流量。

---

## Features / 功能特性

- Real-time upload and download statistics for each process  
  实时显示每个进程的上传与下载流量

- Historical traffic totals by **day**, **month**, and **year**  
  支持按**日**、**月**、**年**统计历史累计流量

- Detailed traffic window with process icons  
  提供带进程图标的详细流量窗口

- English / Chinese interface support  
  支持中英文界面

- Preference persistence across restarts  
  配置项可持久化保存，重启后仍然生效

---

## Requirements / 环境要求

- TrafficMonitor with plugin API version `7`  
  TrafficMonitor，插件 API 版本需为 `7`

- Windows x64  
  Windows x64 系统

- Visual Studio 2022 or Build Tools for Visual Studio 2022  
  Visual Studio 2022 或 Visual Studio 2022 Build Tools

- Build target: `Release | x64`  
  编译目标：`Release | x64`

---

## Install / 安装方法

1. Build the project successfully.  
   成功编译本项目。

2. Copy `ProcessTrafficPlugin.dll` into the `plugins` directory.  
   将 `ProcessTrafficPlugin.dll` 复制到 `plugins` 目录中。

3. Start TrafficMonitor **as Administrator**.  
   以**管理员身份**启动 TrafficMonitor。

4. Open the plugin manager and confirm that the plugin is loaded.  
   打开插件管理器，确认插件已成功加载。

---

## Download / 下载

If you do not want to build the plugin yourself, you can download the prebuilt package from the **Releases** page.  
如果你不想自行编译插件，可以直接在 **Releases** 页面下载预编译版本。

---

## Notes / 注意事项

- This plugin relies on **ETW (Event Tracing for Windows)** to collect per-process traffic data.  
  本插件依赖 **ETW（Windows 事件追踪）** 来采集每个进程的网络流量数据。

- In many cases, TrafficMonitor must be started with **administrator privileges**; otherwise traffic collection may fail and you may see errors such as `StartTrace failed`.  
  在很多情况下，必须以**管理员权限**启动 TrafficMonitor，否则流量采集可能失败，并出现诸如 `StartTrace failed` 的错误。

---

## Build / 编译方法

Open `ProcessTrafficPlugin.sln` in Visual Studio 2022 or Build Tools for Visual Studio 2022, then build with:  
使用 Visual Studio 2022 或 Visual Studio 2022 Build Tools 打开 `ProcessTrafficPlugin.sln`，并按以下配置编译：

- **Configuration:** `Release`  
  **配置：** `Release`

- **Platform:** `x64`  
  **平台：** `x64`

You can also build from the command line with MSBuild:  
也可以通过命令行使用 MSBuild 进行编译：

```powershell
msbuild "ProcessTrafficPlugin.sln" /p:Configuration=Release /p:Platform=x64
