
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
- 带 MSVC v145 工具集和 Windows SDK 的 Visual Studio 或 Build Tools
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

## V2 连接级混合采集

进程统计使用 ETW，并对满足严格交接条件的 TCP 连接采用 EStats。两者不会重复相加，不按网卡比例调整，也不自动选择更大的数值。UDP、短连接、已存在但无法证明边界的连接继续使用 ETW。

- EStats 通过 Windows 原生 IPv4/IPv6 API 采集，仅启用 `TcpConnectionEstatsData`。无需额外驱动、服务或运行库；启用失败时 ETW 继续工作。
- TCP 连接表每 500 毫秒检查一次，累计计数目标每秒读取一次，均不依赖界面刷新。首次读取、计数回退及失败后恢复只建立基线，不补算未知区间。
- 连接验证使用 Windows 原生 TCP 事件的端点、生命周期和序列号。只有完整的 ETW 前缀与两次稳定原生快照一致时才切换；先前字节不重放。缺少身份、序列起点或边界证据时不切换。
- EStats 读取失败、超时或与已验证序列位置不一致时，回退到已知未入账的 ETW 序列区间，裁剪重复及 SYN/FIN 控制序号；无法确定的部分明确标记不完整，不凭估算补齐。
- 历史提交和检查点由独立采集调度触发，不依赖宿主请求或界面刷新；写入失败重试使用同一批次，避免重复累计。
- 连接使用端点、IPv6 作用域、PID、进程创建时间和连接代数识别。表枚举采用包含 PID 和连接创建时间的 `TCP_TABLE_OWNER_MODULE_ALL`，比只有 PID 的表增加连接复用校验。
- 新进程历史按完整程序路径区分同名程序。无法确认归属的字节保留为匿名 PID 数据，不转给其他应用；隐藏匿名项不改变总量。
- 旧历史保留。新记录仍使用每日 TSV 和原有字典；状态文件记录 V2 首条数据时刻。每日 `.quality.tsv` 保存带校验的来源、可信度和实际事件时间范围，不是分钟覆盖率，也不包含另一套可重复相加的流量。
- 元数据损坏只标记来源或完整性未知，不修改有效字节。旧记录没有 V2 元数据，不回填或伪装成新引擎数据。
- 新检查点和追加事务带独立提交 ID 及校验，同一分钟内相同但真实发生的流量不会被误判为重复；冲突恢复会保留原文件并报告异常。
- “诊断”入口显示 ETW 丢失和解析失败、EStats 启用及读取失败、实际来源切换、回退、不完整区间、未归属字节和历史保存状态。零字节异常标记也会保存，暂停刷新仅影响界面。
- Release x64 构建生成链接器映射，并在 DLL 达到或超过 **600,000 字节**时失败。API 7 无卸载回调，采集启动后模块保持映射，完全卸载需退出 TrafficMonitor。

验证中的差异必须按口径解释：[EStats 的 `DataBytesIn/Out`](https://learn.microsoft.com/en-us/windows/win32/api/tcpestats/ns-tcpestats-tcp_estats_data_rod_v0) 包含重传的数据，不含 TCP 首部；网卡计数另含协议及控制流量，因此进程总量不承诺等于网卡总量。已验证 IPv4/IPv6 真实连接的非零前缀切换、旧事件漏采补足和失败回退；具体证据及限制见 `V2_IMPLEMENTATION.md`。

API 7 没有退出回调。插件不会在 DLL 加载锁内等待线程；正常停止会清理自有验证会话，进程强制退出遗留的会话在下次启动时按 GUID、PID 和进程创建时间校验后清理，不停止其他活动会话。

测试源码在 `tests/`；TCP 受控传输、外网对照和负载诊断不会成为 DLL 的运行依赖。测试报告不等同于已证明所有代理、VPN 或 Windows 网络配置都准确。

## 编译方法

使用已安装 MSVC v145 工具集和 Windows SDK 的 Visual Studio 或 Build Tools 打开 `ProcessTrafficPlugin.sln`，并按以下配置编译：

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

