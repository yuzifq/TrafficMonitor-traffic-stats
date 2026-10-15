
# TrafficMonitor 流量统计插件

简体中文 | [English](./README.En.md)

一个用于 TrafficMonitor 的插件，可实时监控**每个进程的网络流量**，并统计累计历史流量。

本项目是 [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 的第三方插件，  
并非 TrafficMonitor 官方项目的一部分。

## 功能特性

- 实时显示每个进程的上传与下载流量
- 独立采集所选网卡的实时速度与分钟历史，支持自动选择或固定网卡
- 支持按时间统计历史累计流量
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
- 流量统计会过滤本机回环通信（`127.0.0.0/8`、`::1` 及对应的 IPv4 映射地址），避免代理、ADB 等程序的本地转发被计为上传或下载。
- 进程统计覆盖 TCP/UDP 网络通信，包含虚拟网卡和隧道流量，合计值不保证等于 TrafficMonitor 所选物理网卡的流量。对比时请使用相同时间段的新增流量。
- 更新后的过滤仅对新采集的数据生效；旧历史记录没有保存连接地址，无法准确扣除其中的回环流量。

## 网卡统计与分钟历史

网卡统计采用与 TrafficMonitor 相同的累计计数求差值方式，通过 Windows `GetIfTable2` 独立读取 64 位收发字节数，不读取官方的实时统计或 `history_traffic.dat`。对比时，请在双方选择同一网卡，并比较相同时间区间的新增流量。自动模式优先选择已连接的硬件网卡，并保持选择稳定；也可手动选择虚拟网卡。

网卡历史保存在进程历史所在目录下的 `network_traffic_history/YYYY/MM/YYYY-MM-DD.tsv`。已完成的采样区间追加写入，当前区间每 10 秒保存检查点；重启时按提交编号恢复，避免重复累计。每天的 `.sum` 是可重建的摘要缓存。程序异常终止时，最后一次检查点之后的数据可能丢失。

- 同一分钟内的连续短采样合并保存，跨分钟或长采样保留真实起止时间，不按比例分配字节数。
- “网卡区间”只累加完全落在所选范围内的记录；“边界待归属”单独显示被查询边界截断的采样流量，不包含在区间合计内。
- “采样覆盖”描述有实际观测的时间，有缺失不代表零流量；长采样会单独标注。
- 时间筛选使用本地时间，存储与网卡导出使用 UTC；导出保留网卡标识、采样精度和边界标记。夏令时重复的本地分钟按 Windows 的时区转换规则解释。
- 网卡选择仅影响之后的采集；历史累计包含此前各次选择的记录。导出按钮可分别导出进程历史或网卡历史。
- 新网卡历史从启用后开始记录，旧进程历史不会补写为网卡历史。ETW 进程数据独立保留，其合计仍可能与网卡计数不同。
- 日、周、月、年快捷范围会跟随当前时间；手动修改时间后停止跟随，也可通过复选框恢复。

Release 构建使用静态运行库 `/MT`、体积优化、全程序优化及未引用代码清理，无需额外安装 Visual C++ 运行库。

## 编译方法

使用 Visual Studio 2022 或 Visual Studio 2022 Build Tools 打开 `ProcessTrafficPlugin.sln`，并按以下配置编译：

- **配置：** `Release`
- **平台：** `x64`

通过命令行使用 MSBuild 进行编译：

```powershell
msbuild "ProcessTrafficPlugin.sln" /p:Configuration=Release /p:Platform=x64
```


# 进入入口

## 右键程序进入插件管理

<img width="587" height="533" alt="image" src="https://github.com/user-attachments/assets/32f020d1-56d8-4d96-8db1-20829a1b6243" />


## 再次右键插件进入流量详情

<img width="710" height="462" alt="image" src="https://github.com/user-attachments/assets/e9ece008-b293-482a-adcd-f8b49aafc6fc" />

<img width="1029" height="644" alt="image" src="https://github.com/user-attachments/assets/56a89174-2dec-43aa-9b30-567efd8f05aa" />

<img width="1116" height="731" alt="image" src="https://github.com/user-attachments/assets/897f1580-47d6-4dfa-a8ca-a6aef7de9399" />

