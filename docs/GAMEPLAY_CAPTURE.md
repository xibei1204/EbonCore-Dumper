# 菜单功能与战斗资源采集

2026-11-21 更新：配置正文优先、菜单/地图/战斗独立名额、依赖优先处理、网络证据与遍历预算分离以及多次采集审计，见 [采集优化](GAMEPLAY_OPTIMIZATION.md)。下述优先级均指资源导出阶段；网络扫描默认在正文保存后执行。

目标采集先直接导出 `/Game/DataTable/MiniMap/` 下已加载的 DataTable，再导出 MiniMapDataAsset 的实例/CDO 及其硬引用，随后采集菜单资产与表、战斗资源及其依赖，最后处理普通资源。这样后续阶段达到总字节预算时，已完成的地图表正文和 manifest 行仍保留。`MiniMapFogBlockData` 表行会输出 `row` 和 `map_block_id`，两者取自实际 DataTable 行名；其他行结构按反射字段原样导出，不假设行名具有相同语义。

专用配置在完成反射和入口验证后自动开启 180 秒采样，按 F8 可重新开启窗口。在窗口内记录匹配版本 HTPlayerState 上的 ClientGameDataRPC 参数包装器原始字节，以及出现时的 OnRecChange_MiniMapFogRec 参数。样本写入 game_rpc_samples.jsonl，状态写入 game_rpc_sample_state.json。采集同时覆盖 PlayerState 的 ProcessEvent 和两个目标 UFunction 的 native 分发入口。两条路径命中同一次调用时去重；native 路径只接受已验证布局的非字节码 FFrame，保留原调用和返回值。

默认收集所有 InMsgID；运行时 FieldNetIndex 301 不是消息 ID。等待窗口显示 armed 后再正常登录，可减少错过首次同步的机会；自动窗口仍无法捕获入口安装之前的调用。采集器不会主动调用 RPC 或构造网络包。capture_stage 区分 ProcessEvent_parameters 和 UFunction_native_parameters，两者都是进程内参数，均不证明线路包格式或地图覆盖完整。BinaryData 编码仍需对照样本分析。

## 使用

在项目根目录运行：

也可直接运行 .\run_gameplay_capture.bat；脚本会在缺少原生构建产物时先构建，再使用 F8 采样配置启动注入器。下面是对应的 CLI 命令：

~~~powershell
python tools/ebon_cli.py inject-and-dump --profile profiles/nte-ue5-gameplay.ini --no-sdk
~~~

新版脚本使用 build-gameplay-observed-sync 产物。启动游戏后，等待日志显示自动 armed，再正常登录和操作；手动采样可在游戏或日志窗口按 F8。文件位于本次注入目录（默认 out/reflection/injected_<时间>_<PID>/）。窗口到期且 window_samples 为 0 时，结合调用和拒绝计数判断原因，再重新按 F8。不要把空样本解释为该消息不存在。

实机窗口 injected_20261121_181237_12720 中，ClientGameDataRPC/Fog native 调用均为 0，但 PlayerState 的 ProcessEvent 观察到了登录同步和传送。game_rpc_sync_capture=1 因而增加三个有实机调用证据的入口：ClientDataLayerRPC、ClientCommonFunRPC 和 SendReplicatedObjectPropertyArrayToClient。前两个按 HottaRpcMessage 的 Type@24 和 ParameterWrapperArray 解析，CommonFun 另保存 InTargetActor；最后一个按反射的容器/属性结构保存属性 FName、所属模块、压缩标记和 payload。压缩数据原样保存，不宣称已解码。

sync_targets 标出每条新增路径的 schema_verified；未通过反射结构校验的路径不采样。window_sync_calls、sync_samples_total 与 captured_by_kind 分别记录调用数、累计排队数及各入口的样本数。新增样本保留真实 kind/function，map_coverage_verified 始终为 false：采到了别的同步入口不代表找到了 MiniMapFogRecord，更不代表完整地图。批量属性最多记录 128 个容器、共 2048 个属性，payload 共用每次调用的字节预算；裁剪均标记 truncated。FName 解析失败时保留 index/number，text 为 null。

状态文件的 installed=true 表示采样器已安装；开启自动采样时状态会从 ready_press_f8 立即进入 armed。native_hooks=2、native_binding_status=rpc_and_fog_installed 表示两个 native 入口均通过校验并安装。fog_callback_target_observed 和 fog_parameter_schema_verified 分别表示 Fog 目标和参数布局验证情况。日志窗口标题持续保留状态，窗口结束时列出调用、参数拒绝、帧拒绝和丢弃数。window_samples 是本窗口排队的记录数，captured_total 是累计排队数，dropped_total 是超过预算的丢弃数。

window_event_calls 与 window_native_calls 分别统计窗口内两个入口的调用；window_target_calls 是目标命中次数，两条入口的命中可能属于同一次调用。window_parse_rejected 是参数不满足布局或过滤条件的次数；window_native_frame_rejected 是 FFrame 不匹配、属于字节码调用或无法读取的次数。window_functions 只保存至多 128 个 PlayerState 事件函数的名称/地址和调用数，不导出无关函数的参数。这些计数使“入口没有调用”和“参数未通过校验”可以区分。F10 执行期间主采集线程忙于导出，状态与样本可能延后刷新；队列仍受内存和条数预算限制。

2026-11-21 修复 PlayerState 派生事件入口的校验：process_event_rva 保留根 UObject 绑定，process_event_playerstate_rva 指定当前哈希对应的派生入口。采样后转发到原派生入口，保留其逻辑。binding_diagnostics 记录实际/预期入口及安装错误；no_verified_playerstate_vtable 是入口不匹配，playerstate_vtable_patch_failed 是入口已验证但安装失败，继续等待不会自动就绪。静态证据与验证范围见 [F8 修复说明](F8_SAMPLER_FIX_2026-11-21.md)。

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
| feature_exports/feature_table_rows.jsonl | 目标表、`/Game/DataTable/MiniMap/` 下已加载表，以及资产引用的已加载表：原始头和每一整行；`MiniMapFogBlockData` 行另标出 `map_block_id` |
| data/.../*.jsonl | 完整反射正文，包括按钮 GameFeatureId / FunctionUnlockId、组件已反射的功能记录和激活字段 |
| reflection/objects.jsonl | 对象路径、名称、Class 路径、Outer、ObjectFlags、是否默认子对象 |
| reflection/network_metadata.json | systematic_game_features_component：三个目标函数的索引观察、父类缓存、字段数量、索引范围 |
| reflection/network_memory.bin / .json | 可离线重放的缓存头和完整字段记录字节 |
| feature_exports/battle_resources.jsonl | 战斗配置、角色/技能资产、蓝图 CDO 正文及装载路径 |
| `data/.../DT_MiniMapFogBlock.jsonl` | 已加载的迷雾区表正文；每行包含由 DataTable 行名给出的 `map_block_id` |
| 战斗导出目录下的 battle_assets.json | 每个包的原始文件位置、SHA-256、装载路径及导出状态 |

组件反射字段为空时，不伪造其原生内部容器。组件正文、已反射祖先字段、Outer 和玩家归属仍保留。若有多个真实资产，全部保留，引用来源用于判断用途；不凭名称断言某一个必然正在使用。

菜单报告在战斗和普通大表导出之前落盘。后续预算耗尽时已完成菜单数据仍保留，resource_state.json 如实记录失败。未解析值保留 _unresolved；字段数为零的父类缓存保留空范围和真实 FieldsBase。

## 网络验收

verify-feature-capture 从保存的内存字节重新解析 FClassNetCache，核对字段指针、类层级、索引边界和兼容标记。修改 JSON 中的数字不能改变重放结果。没有真实缓存、字节缺失、只有 SDK 声明、空 DataTables 或未加载表均不会通过验收。`ClientGameDataRPC` 的反射声明或某个 `InMsgID` 只能作为候选；`GameDataFirstSync` 的处理与记录编码必须由实际调用参数/记录样本验证，当前缓存/布局扫描不会生成这类样本。

退出码：0 表示所列必需数据已采到，1 表示仍有缺项，2 表示文件/格式错误。缓存验证不等于实际数据包兼容性验证。

## 战斗包与装载

export-battle-assets 根据实际引用路径选择包，保留 .uasset/.uexp/.ubulk 等原文件；不把 JSON 或原始 chunk 改名冒充 Unreal 包。蓝图生成类后缀 _C 保留为装载目标，提取文件按所属包匹配。未引用的依赖、动态生成路径和原生服务器代码不会凭空恢复。

IoStore 原始块标记 raw_iostore_only；当前工具没有完整 cooked package 重建。package_bytes_preserved 仅表示原文件已保全；server_load_verified 始终为 false，仍需匹配的服务器引擎、原生类和实际装载测试。

## 验证范围

合成宿主覆盖：实际资产与默认对象区分、DataTables 软引用、LoadedDataTables 硬引用、未加载表保留、完整 Requires/动态 Actions/快捷菜单行、蓝图组件继承、玩家 Outer 链、按钮绑定、零起始网络索引、空父类缓存、字节丢失/元数据篡改、原始战斗包及 sidecar 保全。

这些是工具实现与合成测试，不能当作当前游戏版本的实机资产、网络编号或服务端装载结果。
