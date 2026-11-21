# 角色配置与复制布局采集优化

反射负责发现类型和解码已加载值。完整角色技能配置还可能来自未加载的 cooked 资产、脚本或原生容器；复制布局需要活动网络上下文中的 Parents/Cmds，属性的 RepIndex 不能替代其 handle。增加反射扫描次数本身不能保证补齐这些来源。

## 本次调整

- F10 默认先采集反射元数据和资源正文，再扫描网络。网络阶段刷新内存页并复用本次类型清单，避免重新导出整个 SDK；网络耗时使用单独预算。`resource_defer_network=0` 恢复原先顺序。
- 菜单、地图和战斗分别使用 `resource_feature_max_objects` / `resource_map_max_objects` / `resource_battle_max_objects`，默认各 4096。MiniMapDataAsset（包括保存硬引用的 CDO）作为地图采集入口；加载的 `MiniMapFogBlockData` 表行会同时写出原始行名和 `map_block_id`。
- 硬引用、软引用和生成类的 CDO 继续递归采集；依赖即使没有 Skill/Character 等命名，也保留在战斗报告中。`resource_dependency_depth` 默认 8，范围 1..64。
- JSONL 按文件和状态边界刷盘，取消每行强制刷盘。完成状态发布前检查缓冲输出，失败时保留已完成正文。
- 网络类层级查询使用路径索引，同一次扫描只验证同一候选起点一次。
- `network_capture_graph=0` 保留活动根对象以及通过交叉验证的 cache/header/Parents/Cmds/field 字节；中间图节点只参与遍历。证据预算不再被无关节点消耗，离线验收直接重新验证保存的记录与字节。旧版完整图采集仍可用 `network_capture_graph=1`。

专用配置 [nte-ue5-gameplay.ini](../profiles/nte-ue5-gameplay.ini) 开启以上路线，启动时只采集反射，F10 补采网络。它仍绑定原有客户端 SHA-256；客户端升级需要重新验证配置。没有跨进程保存或复用 UObject 地址、值或网络 handle。

## 使用

重新编译并重新启动采集器与客户端，已有进程中的旧 DLL 不会更新：

```powershell
cmd /c build_native.bat
python tools/ebon_cli.py inject-and-dump --profile profiles/nte-ue5-gameplay.ini --no-sdk
```

进入场景、打开所需角色/技能页面并实际切换角色或使用技能后按 F10。按实际路径比较多次采集：

```powershell
python tools/ebon_cli.py audit-gameplay `
  --capture out/reflection/your_capture/resources/res_1 `
  --capture out/reflection/your_capture/resources/res_2 `
  --out out/reports/gameplay_audit.json
```

`--skip-network` 可只检查正文，避免对历史完整图快照做离线搜索。命令输出正文来源、SHA-256、实际记录数、未解析标记、空值、缺失正文和未加载依赖，以及每次采集分别通过字节验证的复制布局覆盖。比较只允许同一版本；不会拼接不同时刻的字段值或网络 handle。退出码 1 表示观测范围仍不完整，2 表示输入/格式错误；不会凭候选名称或一张非空表声称取得全游戏配置。

完整的已验证 Parents/Cmds、缓存字段和 handle 记录保存在报告的 `captures[].network.validated_records` 中，保留每个来源地址与原始字节；`configurations[].file` 相对于对应 capture，是原始配置正文的位置。

`feature_exports/priority_capture.json` 记录菜单/地图/战斗候选数量、访问数量、名额及深度截断。`capture_pipeline.json` 记录元数据、资源、网络各阶段耗时。`resource_state.json` 在延迟网络阶段结束前保持 collecting；网络失败不会删除已保存正文。

## 网络预算

| 配置 | 默认 | 范围 | 含义 |
| --- | ---: | ---: | --- |
| network_scan_budget_ms | 20000 | 1000..120000 | 图遍历时间 |
| network_max_nodes | 32768 | 1..262144 | 成功读取节点上限 |
| network_max_queue | 65536 | 1..524288 | 入队节点上限 |
| network_max_depth | 4 | 1..8 | 指针遍历深度 |
| network_traversal_mb | 64 | 1..512 | 成功读取的图节点字节 |
| network_evidence_mb | 16 | 1..64 | 持久化证据字节 |
| network_capture_graph | 1 | 0 或 1 | 是否保留中间图节点 |

专用配置设置为 60 秒、65536 节点、131072 队列、深度 5、遍历 128 MiB、证据 16 MiB，并关闭中间图保留。`runtime_scan` 分别记录 traversal_bytes / bytes / layout_probes / graph_retained 和实际耗尽原因。提高预算只能扩大搜索范围；未实例化布局、Iris 的其他结构和未解码的自定义序列化仍可能缺失。

## 根据报告补齐

- `unloaded_asset`：切换相关角色/页面/战斗使对象加载后补采，或沿原始包路径走磁盘资产解析。现有 `export-battle-assets` 保全原始包和 sidecar；IoStore 原始 chunk 仍不等于重建包或反序列化后的配置。
- `dependency_body_not_exported` / 名额耗尽：优先增加战斗名额或降低无关候选，检查深度截断；不要直接把未导出的候选当作不存在。
- `partial` / `empty_values`：查看正文 `_unresolved` 与类型结构。未反射的原生字段、脚本 VM 数据和空 CDO 需要对应序列化/脚本/原生容器解析器，继续扫描同一反射字段不会生成缺失信息。
- 网络 `not_observed`：确认存在实际 NetDriver/ActorChannel 和目标对象的复制流量，再补采。完整 Parents/Cmds 表仍需要实际数据包验证其兼容性。

合成回归覆盖：深层布局不被中间节点填满证据预算、遍历预算独立生效、记录篡改被离线拒绝、菜单与战斗名额隔离、配置及无关键词依赖优先、网络失败后保留正文。合成通过不能代替当前客户端实机验收。
