# 角色定向采集检查（2026-11-21）

## 已核对的证据

- 静态文件检查：现有十份 `resources/res_N/resource_manifest.jsonl` 均没有 `player_090_elyms` 的角色正文。最新 `injected_20261121_173339_10668/resources/res_1/reflection/types.jsonl` 没有该生成类；其 UObject 清单中的 Elyms 骨架不等于角色类已经加载。
- `DT_Character[1090].ElementData.value.CharacterActorClass` 指向 `/Game/Blueprints/Character/Player/player_090_elyms.player_090_elyms_C`。有角色表和头像不能证明已有 GrantedAbilities、PassiveAbilities 或复制布局。
- 最新优先级报告的战斗候选为 63109，8192 个访问名额已耗尽。此前角色根对象与骨架、动画等广泛依赖共用这份名额，不能保证已加载的角色配置优先写出。

## 采集契约

读取已加载对象，先保存已观测的 `DT_Character` 与全部玩家角色根对象，再跟随其依赖，使用独立 `resource_character_max_objects` 名额。保留菜单、地图、战斗阶段各自的计数。未加载角色仍明确报告，采集器不调用加载函数。

`audit-character` 必须从实际角色表解析目标类，并分别记录每次采集的生成类、默认对象/运行实例正文、两份技能列表、武器引用与复制布局。只接受同一构建；不得跨次拼接默认值和网络 handle，不得用基础 Character 布局代替目标生成类。没有完整布局字节时不得报告资料齐全。

## 验收边界

目标：合成宿主证明角色根对象不会被广泛战斗候选和依赖挤掉，离线检查准确区分角色表、已加载类、配置正文和实际布局。新游戏 dump 与 Elyms 换角由用户后续操作验收。

本轮不启动或注入客户端，不创建提交。改动前的工作区文件及 SHA-256 保存在 `.work/character-capture-fix-*/before/` 与 `baseline.json`，该目录不提交。

## 重新采集

需要补齐未加载的可操作角色类时，优先使用新增的 [角色定向加载入口](CHARACTER_LOADER.md)。以下普通 F10 路径仍只读取已经加载的对象。

运行根目录 `run_gameplay_capture.bat`。该入口本轮使用 `build-gameplay-character-capture/` 中的新构建，保留之前的构建和快照。已经加载的旧 DLL 不会随磁盘构建更新，需要由用户重新启动本次采集流程。

打开目标角色的详细预览或能创建其实际实例的界面，等加载结束后按 F10。仅右侧头像或模型预览也可能只加载骨架；这时采集器不会自行执行加载或切换函数。不要把“在当前坏服上成功切角”作为重新采集的前置条件。

对新的 F10 目录执行：

```powershell
python tools/ebon_cli.py audit-character --capture out/reflection/your_capture/resources/res_1 --character player_090_elyms --out out/reports/elyms-character-audit.json
```

报告列出每次采集的 `class_loaded`、配置正文及 SHA-256、已解析的技能列表/武器引用、精确类布局和 `missing`。`--skip-network` 只检查正文，结果明确保持不完整。退出码 0 仅表示所检查的配置与布局资料在同一份采集中齐全，1 表示仍缺资料，2 表示输入或格式错误，均不代表真实客户端换角已通过。

## 已执行验证

- `cmd /c "build_native.bat build-gameplay-character-capture"`：11 个原生目标构建成功；最终日志 `.work/character-capture-build-final.log`。
- 设置 `EBON_TEST_BUILD_DIR=build-gameplay-character-capture` 后运行完整 `unittest discover`：137 项通过，36.310 秒，退出码 0；日志 `.work/character-capture-tests-final.log`。覆盖角色名额与根对象优先、角色表优先、未加载角色缺项、精确角色/武器布局、跨次采集不得拼接及输入路径保护。
- 对最新现有 F10 快照执行 `audit-character --character player_090_elyms`：重新验证现存网络字节后退出码 1，明确缺 `generated_class_not_loaded`、`character_body_not_observed`、`exact_character_layout_not_observed`；报告 `.work/elyms-character-audit-20261121.json`。
- 同一快照的薄荷正文能被识别为包含两份完整技能列表；未运行网络检查的结果仍明确保持不完整。
- 未进行新的游戏采集，未验收 Elyms 实际换角。最终源码和新 DLL 的 SHA-256 在 `.work/character-capture-artifacts.json`，旧构建与输入快照保留。

## 用户 21:40 补采结果

用户提供 `injected_20261121_212921_27912/resources/res_1`。离线检查确认：新角色阶段已运行，8 个根候选优先预留，累计访问 2048 个对象；`resource_state.json` 为 `complete_with_unresolved`。该状态表示本次采集完成，不表示指定角色资料齐全。

本次新取得 Elyms 武器的 CDO、展示动画蓝图等 11 份相关正文，但 F10 自身的 `reflection/types.jsonl` 仍没有 `/Game/Blueprints/Character/Player/player_090_elyms.player_090_elyms_C`。角色优先名额耗尽不能解释一个未加载生成类的缺失。

`DT_Character[1090]` 分别引用可操作类 `CharacterActorClass` 和展示类 `UISimpleCharacterActorClass=/Game/Blueprints/Character/PlayerUIShow/PlayerUIShow_090.PlayerUIShow_090_C`。预览模型、武器 CDO 和 UI 动画不能替代前者的技能默认值与实际网络布局。因此只打开详情预览不足以补齐换角资料，不应要求用户在当前无法换角的服务端上先成功切角，或继续重复相同 F10。

命令 `audit-character --capture out/reflection/injected_20261121_212921_27912/resources/res_1 --character player_090_elyms` 重新验证网络记录后退出码 1，仍明确报告生成类未加载、角色正文缺失、精确角色布局缺失。报告在 `.work/elyms-character-audit-212921.json`，来源文件 SHA-256 在 `.work/elyms-character-review-212921.json`。本轮只读取用户的采集结果，没有重新启动或访问游戏进程。Elyms 的服务端换角仍未完成。
