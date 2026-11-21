# 菜单功能与战斗资源采集

2026-11-21 更新：配置正文优先、菜单/战斗独立名额、依赖优先处理、网络证据与遍历预算分离以及多次采集审计，见 [采集优化](GAMEPLAY_OPTIMIZATION.md)。下述菜单/战斗优先级均指资源导出阶段；网络扫描默认在正文保存后执行。

本次改动增加目标优先采集。先处理菜单资产、引用配置、功能表、实际组件和按钮，再处理战斗资源及其依赖，最后处理普通资源。采集器只读访问已加载对象，不调用游戏函数加载未加载资产。

## 使用

在项目根目录运行：

~~~powershell
python tools/ebon_cli.py inject-and-dump --profile profiles/nte-ue5-gameplay.ini --no-sdk
~~~

进入可操作场景，打开主菜单和相关角色页面，等待资源加载后按 F10。专用配置开启 resource_feature_only，跳过无关对象的批量导出。普通配置仍执行全量导出，同样优先采集目标数据。配置继续绑定原有 HTGame.exe 哈希，不能直接用于未重新验证的客户端版本。

长 Outer 链造成的超长文件名会保存为 data/_long/object_<稳定哈希>.jsonl；JSON 与清单仍保留完整对象路径。专用配置将网络扫描时间预算设为 60 秒，资源采集预算设为 300 秒。重新编译或更换 DLL 后必须重新启动游戏加载新版，已经运行的旧 DLL 不会自动更新。

F10 只采集已经存在的对象。若菜单页面打开后仍没有实际 SystematicGameFeaturesComponent，不能通过多按 F10 或只加载蓝图类来取得该组件的网络编号；需要确认本客户端是否启用这套系统及其真实创建条件。未解析的 FText 也不会仅因重复采集自动变为已解析。

~~~powershell
$Capture = 'out/reflection/your_capture/resources/res_1'
python tools/ebon_cli.py verify-feature-capture --capture $Capture
python tools/ebon_cli.py export-battle-assets --capture $Capture --client 'C:/Program Files/Neverness To Everness' --out 'out/resources/your_battle_assets'
~~~

加密容器沿用 EBON_AES_KEY 环境变量。Oodle 压缩沿用 --oodle-dll / --oodle-sha256 选项。

## 输出

| 文件 | 内容 |
| --- | --- |
| feature_exports/systematic_game_features.json | 实际资产候选、目标表、非默认组件、按钮、软/硬引用及未采到项 |
| feature_exports/feature_table_rows.jsonl | 目标表与 DataTables/LoadedDataTables 引用的已加载表：原始头和每一整行，不裁剪 ID、Requires、Actions、UINameToOpen、快捷菜单字段 |
| data/.../*.jsonl | 完整反射正文，包括按钮 GameFeatureId / FunctionUnlockId、组件已反射的功能记录和激活字段 |
| reflection/objects.jsonl | 对象路径、名称、Class 路径、Outer、ObjectFlags、是否默认子对象 |
| reflection/network_metadata.json | systematic_game_features_component：三个目标函数的索引观察、父类缓存、字段数量、索引范围 |
| reflection/network_memory.bin / .json | 可离线重放的缓存头和完整字段记录字节 |
| feature_exports/battle_resources.jsonl | 战斗配置、角色/技能资产、蓝图 CDO 正文及装载路径 |
| 战斗导出目录下的 battle_assets.json | 每个包的原始文件位置、SHA-256、装载路径及导出状态 |

组件反射字段为空时，不伪造其原生内部容器。组件正文、已反射祖先字段、Outer 和玩家归属仍保留。若有多个真实资产，全部保留，引用来源用于判断用途；不凭名称断言某一个必然正在使用。

菜单报告在战斗和普通大表导出之前落盘。后续预算耗尽时已完成菜单数据仍保留，resource_state.json 如实记录失败。未解析值保留 _unresolved；字段数为零的父类缓存保留空范围和真实 FieldsBase。

## 网络验收

verify-feature-capture 从保存的内存字节重新解析 FClassNetCache，核对字段指针、类层级、索引边界和兼容标记。修改 JSON 中的数字不能改变重放结果。没有真实缓存、字节缺失、只有 SDK 声明、空 DataTables 或未加载表均不会通过验收。

退出码：0 表示所列必需数据已采到，1 表示仍有缺项，2 表示文件/格式错误。缓存验证不等于实际数据包兼容性验证。

## 战斗包与装载

export-battle-assets 根据实际引用路径选择包，保留 .uasset/.uexp/.ubulk 等原文件；不把 JSON 或原始 chunk 改名冒充 Unreal 包。蓝图生成类后缀 _C 保留为装载目标，提取文件按所属包匹配。未引用的依赖、动态生成路径和原生服务器代码不会凭空恢复。

IoStore 原始块标记 raw_iostore_only；当前工具没有完整 cooked package 重建。package_bytes_preserved 仅表示原文件已保全；server_load_verified 始终为 false，仍需匹配的服务器引擎、原生类和实际装载测试。

## 验证范围

合成宿主覆盖：实际资产与默认对象区分、DataTables 软引用、LoadedDataTables 硬引用、未加载表保留、完整 Requires/动态 Actions/快捷菜单行、蓝图组件继承、玩家 Outer 链、按钮绑定、零起始网络索引、空父类缓存、字节丢失/元数据篡改、原始战斗包及 sidecar 保全。

这些是工具实现与合成测试，不能当作当前游戏版本的实机资产、网络编号或服务端装载结果。
