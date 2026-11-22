# 角色定向加载与自动导出

## 使用

双击根目录 `run_character_capture.bat`，默认选择 Elyms（`DT_Character` 行 1090）。按窗口提示从自己的启动器重新启动本地客户端，进入场景后回到这个终端按一次 Enter。无需再打开角色预览或按 F10。

入口使用独立构建 `build-character-loader/` 和 `profiles/nte-ue5-character.ini`，旧进程中的 DLL 不会自动更新。启动时从已完成 F10 快照的实际 `DT_Character` 解析目标可操作类，显示来源目录和完整路径。没有该表行时停止，允许用 `--source <resources/res_N>` 明确指定来源。

```powershell
.\run_character_capture.bat --character 1090
# 已有本次新版采集器时可复用；不向旧 DLL 补装功能。
python tools/capture_character.py --capture out/reflection/character_your_capture --character 1090
```

输出在 `out/reflection/character_*/characters/char_<request_id>/`：

- `character_load_state.json`：加载/导出阶段或具体失败原因。
- `resources/res_1/`：加载后重新取得的反射、目标角色正文/依赖及实际网络证据。
- `character_audit.json`：自动检查同一份采集中的生成类、技能配置、武器和精确复制布局，并保存源表 SHA-256。

退出码 0 表示所检查资料齐全，1 表示加载失败或仍缺资料，2 表示输入或控制流程错误。类加载成功和资源导出完成都不代表 Elyms 实际换角已通过。没有活动复制上下文时，目标类布局仍可能为 `not_observed`；工具不生成角色 Actor、不改变接管、不用其他角色布局代替它。

## 实现和证据

本入口是用户明确选择的主动加载模式，普通 F10 资源读取保持原行为。加载通过当前构建已捕获的 `KismetSystemLibrary.MakeSoftClassPath` 和 `LoadClassAsset_Blocking` 反射函数完成，使用当前进程自己创建的 FName 与 SoftClassPath，不复用旧进程名称索引或 UObject 地址。

参数来自 `injected_20261121_212921_27912/resources/res_1/reflection/types.jsonl`：两函数参数区均为 48 字节，SoftClassPtr 为 40 字节、返回类位于 40；SoftObjectPath 为 32 字节，包含两个 FName 的 TopLevelAssetPath 和 Utf8StrProperty 子路径。离线代码核对使用 `.work/character-loader/HTGame.analysis.exe`，规格中的分析副本 SHA-256 与原安装模块 SHA-256 分开记录，未将修改后的分析镜像当作原文件。

函数 RVA、代码前 16 字节及参数契约见 `character-loader.spec.json`。每次绑定重新核对模块哈希、反射参数类型/偏移、函数入口字节、UFunction 原生指针以及 Kismet CDO 的 ProcessEvent 槽。配置不匹配时停止调用。

线程在运行时同时核对三个条件：本进程可见 `UnrealWindow`、所属线程名恰为 `GameThread`、消息回调执行线程等于选定线程。仅对该线程安装短时 `WH_GETMESSAGE` 回调并投递 `WM_NULL`；调用完成后移除。没有满足条件的窗口时明确失败，不用“最早创建的线程”猜测。请求在执行前超时就取消，不会稍后意外执行；已经运行的同步引擎加载不强行中断。

完成加载后重新采集反射值，角色模式只为目标类、角色表及已加载依赖执行资源优先遍历，避免广泛战斗候选占用这轮资源名额。网络证据仍由原扫描器获得，并由原审计器重新验证字节。

## 验证范围

原生合成宿主验证窗口线程派发、参数传递、入口指纹/参数偏移拒绝与超时后不执行。Python 回归验证实际表选择、构建一致性、请求原子发布、已有请求保护和导出后自动审计。测试仅操作自建宿主；本轮没有启动、注入或加载真实客户端。真实客户端的线程名/窗口绑定、Elyms 类加载、默认对象保留与复制布局仍待用户运行验证。

已执行：

- `build_native.bat build-character-loader`：12 个原生目标构建成功，未报告编译告警；日志 `.work/character-loader/build-final.log`。
- 对该构建运行完整 Python 回归：141 项通过，39.353 秒，退出码 0；日志 `.work/character-loader/tests-final.log`。随后完善导出失败状态，并重建及运行 DLL、角色请求、资源导出的相关 9 项回归，全部通过，9.294 秒；日志 `tests-incremental.log`。
- `verify_character_loader_image.py --image .work/character-loader/HTGame.analysis.exe`：三处代码入口指纹与已记录分析镜像匹配；报告 `image-verification.json`。这只是离线代码核对。
- 从用户 21:40 F10 的实际角色表选择 1090，正确取得 Elyms 的可操作蓝图类和表正文 SHA-256；没有通过猜测角色名称生成装载路径。

上述日志、改动前副本和最终源码/新 DLL 指纹在 `.work/character-loader/`，不提交；原有构建、用户快照、运行服务端和客户端文件保留。没有创建提交、推送或修改 Codex 的 REA 配置。
