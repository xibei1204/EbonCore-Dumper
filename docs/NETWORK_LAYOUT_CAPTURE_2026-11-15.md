# 复制布局补采修复（2026-11-15）

## 确认的缺口

旧场景快照 `out/reflection/focused_live_f10_32768_20261114_231312/network_snapshot_1` 重验得到 44 个类网络缓存、11 个完整布局、346 条非 Return 句柄。PlayerState、Character、AbilitySystemComponent、GameState 和 WorldDataLayers 的完整布局均未观察到。类缓存字段索引不代表复制命令布局。

这份快照保存 14,314,784 字节，地址去重后约 7.6 MB。扫描受到预算截断；WorldDataLayers 候选保存了 15 条有效 Parents，但其 29 条 Cmds 所指内存完全没有保存，不能离线补造。

## 修复

- 网络证据按虚拟地址范围去重，保存量严格不超过 16 MiB。保留 32,768 节点、65,536 排队节点、4 层指针扩展和 20 秒上限，另记录具体预算原因、读失败和证据不全记录数。
- 解码器返回实际 Parents/Cmds 的指针、数量和步长。保存准确的 `count * stride`，完整保存 header 和两张表后才发布布局；移除遍历附近数组后一律乘 72 的保存方式。
- 扫描小分配尾部的指针；入队时按地址和请求范围去重，排除已知反射字段，减少反射链消耗。
- 反射 detail 递归记录容器内部 FProperty 的地址、偏移、元素大小和静态数组维度。原生与 Python 解码均登记 `.inner`、`.key`、`.value`、`.underlying`，使动态数组命令能指向真实内部字段。旧快照缺少这些地址时保持未知。
- 离线扫描按虚拟地址对齐，并跨已保存分片读取边界字段，兼容去重后的短块和非对齐边界。
- 新增只读类级覆盖报告，按继承关系列出具体 Owner 类与每张表的实际/声明 Parents、Cmds 数量；报告生成不修改原始采集目录。
- DLL 回归发现资源补采在完整反射成功后仍错误套用部分反射最小规模阈值，已修复该终态判断。

## 使用

候选构建：`build/candidate-network-layouts`。

```powershell
python tools/ebon_cli.py inject-and-dump `
  --injector build/candidate-network-layouts/EbonDebugInject.exe `
  --dumper build/candidate-network-layouts/EbonDumper.dll `
  --profile profiles/nte-ue5.ini --dump-out out/reflection/<新目录> --no-sdk

# 初次采集结束并进入可操作场景后
.\tools\request_network_capture.ps1 -Capture out/reflection/<本次目录>

# 报告必须输出到原始采集目录之外的新目录
python -m tools.ebon.network_coverage `
  --capture out/reflection/<本次目录>/network_snapshot_1 `
  --out out/reports/<新目录>
```

每个目标以实际 Owner 类的完整 Parents/Cmds 为验收单位。测试通过和扩大扫描范围不能代替实机捕获；包收发兼容性仍须另验。

## 验证

完整 MSVC 构建通过。原生反射自测覆盖准确命令区边界、重叠证据去重、小分配尾部、严格字节预算及内部 FProperty 命令；资源和 AES 自测通过。Python 全仓 76 项通过，新增覆盖报告后网络定向 16 项通过；详细日志位于候选构建目录。

## 本次实机结果

目录：`out/reflection/network_layouts_20261115_004927`。启动快照成功，场景补采为 `network_snapshot_1`；模块 SHA-256 与 profile 匹配。

场景原始记录有 25 个布局（含不同起点的重复记录），离线按表重解并逐字节验证后保留 **21 个完整布局、0 个部分布局、1,522 条非 Return 句柄**，另有 3 个类网络缓存和 144 条字段索引。目标覆盖如下：

| 实际 Owner | Parents | Cmds |
| --- | ---: | ---: |
| BP_PlayerStateBase_C | 152/152 | 343/343 |
| player_046_male_C | 83/83 | 143/143 |
| player_033_adler_C | 83/83 | 143/143 |
| Player_019_Mint_C | 83/83 | 143/143 |
| Player_020_Haniel_C | 83/83 | 143/143 |
| HTAbilitySystemComponent | 37/37 | 108/108 |
| BP_GameStateBase_C | 36/36 | 119/119 |
| WorldDataLayers | 15/15 | 29/29 |

逐类与逐表证据：`out/reports/network_layouts_20261115_004927/network_coverage.json` 和 `.md`。扫描根 123、访问节点 13,974、去重保存 9,575,632 字节；仅触及时间预算，`incomplete_records=0`。这些数量仅覆盖该场景实际观察到的具体 Owner，不表示全部 176 个已加载 Character 类都已恢复。

场景全局反射有 12 条动态蓝图校验错误，状态 `failed`，不能替代完整 SDK 的布局来源；已重验的网络表来自它保存的有效字段和原始内存。SDK 使用成功启动快照生成声明，并单独引入这个同哈希场景的网络映射。`packet_mapping_verified=false`。

SDK 输出：`out/sdk/CppSDK_NetworkLayouts_20261115_004927`。

SDK manifest 验收数量：Parents **802/802**、Cmds **1,626/1,626**。使用 MSVC `/bigobj /std:c++17 /W4 /WX` 编译、链接全量 SDK 成功，CompileCheck 执行返回 0；日志为 `build/network-layouts-sdk-compile.log`。

最终原生构建为 `build/candidate-network-layouts-final`（保留实机已加载的候选文件）。仓库另有并行资源改动，因此最终测试使用该目录重新构建的资源自测，不复用较早可执行文件。

最终全仓 Python **79 项通过**，反射、资源、AES 原生自测均通过。日志为 `build/candidate-network-layouts-final/python-tests.log`。
