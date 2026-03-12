
# TrafficMonitor 流量统计插件

简体中文 | [English](./README.En.md)

一个用于 TrafficMonitor 的插件，可实时监控**每个进程的网络流量**，并统计累计历史流量。
本项目是 [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 的第三方插件，  
并非 TrafficMonitor 官方项目的一部分。

## 功能特性

- 实时显示每个进程的上传与下载流量
- 支持按**日**、**月**、**年**统计历史累计流量
- 提供带进程图标的详细流量窗口
- 支持中英文界面
- 配置项可持久化保存，重启后仍然生效

## 环境要求

- TrafficMonitor，插件 API 版本需为 `7`
- Windows x64 系统
- Visual Studio 2022 或 Visual Studio 2022 Build Tools
- 编译目标：`Release | x64`

## 安装方法

1. 成功编译本项目。
2. 将 `ProcessTrafficPlugin.dll` 复制到 `plugins` 目录中。
3. 以**管理员身份**启动 TrafficMonitor。
4. 打开插件管理器，确认插件已成功加载。

## 下载

如果你不想自行编译插件，可以直接在 [Releases](../../releases) 页面下载预编译版本。

## 注意事项

- 本插件依赖 **ETW（Windows 事件追踪）** 来采集每个进程的网络流量数据。
- 在很多情况下，必须以**管理员权限**启动 TrafficMonitor，否则流量采集可能失败，并出现诸如 `StartTrace failed` 的错误。

## 编译方法

使用 Visual Studio 2022 或 Visual Studio 2022 Build Tools 打开 `ProcessTrafficPlugin.sln`，并按以下配置编译：

- **配置：** `Release`
- **平台：** `x64`

也可以通过命令行使用 MSBuild 进行编译：

```powershell
msbuild "ProcessTrafficPlugin.sln" /p:Configuration=Release /p:Platform=x64
