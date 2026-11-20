# F10 资源值导出

## 使用

重新使用 `build/EbonDumper.dll` 注入客户端，启动反射采集完成后，日志显示 `F10 resource dump ready`。游戏在前台时按 F10，结果写到本次采集目录的 `resources/res_1`、`resources/res_2` 等目录。F10 监听持续到进程退出，长按不会连发；也可创建 `resource_capture.request` 触发相同路径。原始反射采集结果保留在父目录。

`resource_state.json` 是资源采集状态。仅 `complete` 或 `complete_with_unresolved` 且 `success=true` 表示完成了可用采集；`no_resources` 表示没有取得可导出的资源，`failed` 表示预算、超时、文件写入或反射验证失败。每类覆盖使用 `observed_partial` / `not_observed`，不推断未加载资源不存在。

## 数据来源

按 F10 时重新采集对象与类型，不复用启动时的对象地址清单。反射元数据走独立快照缓存，资源值通过受保护的只读内存访问器读取，不调用 ProcessEvent，不触发资产加载。每个资源在读取前检查对象索引、类指针和名字，读取后复核容器头及分配位图。采集不是冻结游戏后的原子快照，运行中的配置修改仍可能影响观察值。

支持的值包括有符号/无符号整数、浮点、布尔掩码、FName、FString、枚举、继承结构、FInstancedStruct、对象引用、可反射软引用路径、定长数组、TArray、TSet 和 TMap。映射使用键值对列表，保留非字符串键。未知结构、未确认对齐、未加载或编码的对象引用、异常数值、读失败及容器变化保留为 `_unresolved`，不会填入伪造默认值。

DataTable 使用反射的 RowStruct 解码实际表行。当前布局中 RowStruct 位于 `0x28`，其后至 `0x80` 的空隙容纳 80 字节 RowMap；默认候选从 RowStruct 后 8 字节开始。候选必须通过稀疏数组头、分配位图、全部有效行名及行指针校验。内联与外置位图都支持，空洞不会作为行导出。`resource_datatable_rowmap_offset` 可以在同版本 profile 中显式覆盖候选偏移。此布局已通过合成内存验证，仍需要真实客户端采集确认。

DataAsset 同时导出默认对象与实例，保留 `is_default` 标记。不能仅筛选 `/Game/`：已有快照中的 HTLotteryManager 实例位于 `/Engine/Transient...`。卡池模块的保底、卡池类型、SSR 清单、角色/皮肤 ID、消耗、额外奖励和描述字段均按反射实际值读取，实例引用保留对象路径供关联。

## 六类索引

| 目录 | 数据范围与实际命名 |
| --- | --- |
| `characters` | Character、RoleLevel、Upgrade、Breakthrough、Awaken、Equipment、Skill 等 |
| `gacha` | Gacha、Lottery、CardPool、Gashapon、Monopoly 等 |
| `activities_systems` | Activity、SignIn、CheckIn、Shop、System、Config 及未命中其他类别的资源 |
| `story_quests` | Quest、Mission、Story、Dialog、CharacterFile 等 |
| `localization` | StringTable、TextTable、Dialog、文本候选及 FString/FText 汇总 |
| `character_skins` | Fashion、Appearance、Dye、Decoration、Costume、Skin 等 |

分类属于基于命名的索引，允许多类关联，不等同于游戏官方资源分类。角色皮肤主要使用 Fashion/Appearance/Dye/Decoration 命名，单筛 Skin 会漏项。SkinnedMesh、Skinning、SkinWeight、SkinCache 不作为皮肤关键词。

## 文本边界

FString 按 UTF-16 读取并校验长度、容量和终止符，转换为 UTF-8 JSON。`localization/text_values.jsonl` 汇总字符串及来源字段路径，数组和映射包含元素索引。

当前客户端的 FText 大小为 16 字节。已有快照没有保存足够的原生文本内存，不能确认 ITextData 内部布局或源文本/显示文本语义。导出器保存原始字节，并在有界范围内收集通过 FString 校验的候选，包含偏移或间接指针路径。所有这些值仍带 `_unresolved`，不把候选当成已确认译文。

StringTable 在原生引用的前 512 字节内寻找唯一、通过全部有效 FString 键校验的稀疏映射，再收集每项前 128 字节内有效的字符串候选。结果标注 `inferred_candidates`、`translations_verified=false`，并保留偏移供后续实机确认。无法唯一确定、空表、损坏或变化的表写入未解析清单。当前客户端 Client 目录没有 loose `.locres`，本实现未解包或解压容器中的本地化资产。

## 预算与验证

profile 默认每容器最多 100,000 项、单字符串 65,536 个 UTF-16 码元、2,000,000 个值、256 MiB 资源 JSON 输出、16 层递归；反射发现和资源读取各有 180 秒期限。资源 JSON 的总字节预算包含索引、文本汇总与未解析详情，不包含单独保存的反射元数据。触及预算写出失败状态，保留已写出的诊断文件。

原生合成测试覆盖实际表行值、中文字符串、布尔掩码、数组、嵌套结构、带空洞的映射、内联/外置位图、损坏位图拒绝、瞬态 DataAsset、跨空中间基类的祖先字段、文本不确定性标记与预算失败。DLL 宿主测试通过文件请求连续执行两次采集，验证新目录和原始启动采集独立。Python 测试进一步解析生成的 JSON/JSONL，检查 Unicode、键值和状态语义。

本轮同时修正了反射自测的节点预算用例：原用例构造 2,100 节点单向深链，但扫描器原有深度上限为 4，最多访问 5 节点。随后扩展为 10,000 个独立根，验证新扫描预算可超过此前的 8,192 节点上限。

本轮验证结果：默认 `build/` 的 10 个原生产物全部构建成功；完整 Python 回归 57 项通过，最终祖先字段与预算修复后相关 5 项再测通过。反射、AES、资源原生自测全部通过，资源自测输出的全部 JSON/JSONL 可解析。最终合成证据保存在 `out/verification/res-f11-final-native/`，替换前默认可执行文件备份在 `out/verification/f11-prechange/`。

当前热键已改为 F10；轮询同时检测按下沿和 `GetAsyncKeyState` 按键事件位，并将间隔缩短到 10 ms。F10 可在游戏窗口或该游戏对应的独立 EbonCore 日志窗口处于前台时触发；不接受其他进程窗口。PID 绑定通过启动日志查看器时拿到的子进程 ID 实现。用户提供的日志显示旧逻辑检测到 F10，但只接受游戏 PID；当独立日志窗口在前台时，日志 PID 被误判成无关窗口。通过 `resource_capture.request` 的在线触发能进入采集器，但刷新反射时因场景对象变化失败，未生成可用资源数据。

修复版会对新反射清单做最多三次重试。若全局成功状态仍因瞬态对象变化为 false，但 NamePool/ObjectArray、属性布局以及最小对象/类型/属性数量都通过验证，就继续资源读取。读取过程继续校验每个对象当前索引、类与名字、容器头及位图；缺字段和对象变化记入 `unresolved.jsonl`。反射错误同步保存到 `reflection_attempts.json` 和嵌套反射快照状态，资源结果会标记 `complete_with_unresolved`。若基础布局或数量门槛不满足，则停止并写出每次尝试的错误，不使用不完整清单解码。

修复热键日志窗口误判后的候选构建位于 `build/candidate-focused-runtime-f10-logfix`；随后合入 32,768 节点扫描限制、F10 日志窗口 PID 配对、短按边沿检测及瞬态资源反射容错的版本位于 `build/candidate-focused-runtime-f10-32768-retry`。完整原生构建、反射/DLL/AES/资源自测及相关网络、资源、DLL Python 测试通过。用户提供的前台 PID 日志已确定误判根因，但 PID 配对版仍需真实按键触发确认。已有登录/更新器快照不覆盖全部功能界面；应进入相关界面后补采，并以各次 `resource_state.json` 和 `unresolved.jsonl` 判断覆盖及缺项。

## 2026-11-15 实机结果复核

最新采集位于 `out/reflection/fixed_f10_20261115_001920/resources/res_1`。日志确认请求标记触发了转储；本次没有留下 F10 按键检测记录，因此 F10 实际按键是否已修好仍未验收。

转储包含 301 个 DataAsset 和 47 个 DataTable 清单项，读取 41,070 行表数据、78,624 条文本值；分类计数允许重复归类。`resource_state.json` 为 `failed`，因为达到 `resource_max_values=2,000,000`，输出约 118.7 MB。它是部分结果，不能当作全量配置。结构化 JSON/JSONL 均已复核通过。

可用内容包括 400 行的 `DT_FT_PlayerModifyPackData`、18 行的 `DT_EquipmentModifyData`、13,987 行的 `DT_FT_Armor`，以及多张任务/对话表。`DT_FT_Armor` 有 195,818 个未解析值，部分对话表每行都有未解析字段。抽卡类命中 6 个资源，但都是 Default 对象；`HTLotteryManager.ModuleDefaultMap`、`NextLotteryCharacterIDS`、`SSRList` 和 `CumLotteryConfig.LotteryPoolIDs` 均为空，没有观察到当前活动卡池实例。

未解析详情统计 229,000；`unresolved.jsonl` 只保存最多 1,000 条样例。已看到的常见限制包括不支持的 `Utf8StrProperty`、未知 Set/Map 对齐和未确认 FText 语义。`activities_systems/resources.jsonl` 有一处记录分隔损坏；已从权威 `resource_manifest.jsonl` 重建为 258 行，并把原件保存在同目录 `resources.jsonl.corrupt_original`。

随后在 `out/reflection/network_layouts_20261115_004927/resources/res_1` 又采集到 343 个资源项（318 DataAsset、25 DataTable）、37,586 行和 62,274 条文本相关值。该次用了普通 180 秒预算，因资源读取 deadline 到期而标记失败；已写出的结构化文件与分类链接仍全部有效，资源文件总量约 396 MB。这次运行也命中了更多当前地图和角色对象。若要继续完整化，使用 `profiles/nte-ue5-resource-export.ini` 的 600 秒、500 万值、1,024 MiB 预算重新运行。

按用户提示的 `F10 press observed, but foreground pid=23044 game pid=33272` 已确认：23044 是标题绑定 33272 的 EbonLogConsole 进程。游戏把 F10 输入事件发给当前前台窗口，因此只判断游戏 PID 会漏掉日志窗口前台时的短按。最新版改为允许游戏 PID 或这次启动创建并绑定的日志查看器 PID 前台触发，其他进程仍会忽略；此最终前台配对修复已通过合成关系测试，仍待一次真实 F10 按键触发确认。
