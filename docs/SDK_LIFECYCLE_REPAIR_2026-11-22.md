# SDK 生命周期与覆盖修复

本轮按“先完成代码和已有快照修复”执行。合并了同一模块 SHA-256 的启动快照 `injected_20261122_153811_7580` 与场景快照 `injected_20261122_165347_20368`，主快照为后者。

| 项目 | 原 SDK | 本轮结果 |
| --- | ---: | ---: |
| 已生成类型 | 80,324 | 80,473 |
| 类与结构体布局 | 28,749 | 28,769 |
| 缺少的旧类型 | 149 | 全部从启动快照补回 |
| 因生命周期变化剔除的类型 | 40 | 40；现有快照没有有效替代记录 |
| 未解决字段语义 | 293 | 142；同布局旧快照补回目标引用 |
| 对齐头字节独立复验 | 0 | 256 |
| 缺少对齐值的布局 | 28,749 | 28,513 |
| RPC 已观察字段索引 | 1,204 / 1,497 | 1,204 / 1,497 |
| 包级收发验证 | 未完成 | 未完成 |

## Runtime

路径缓存保存 `ObjectHandle`，包含对象数组索引、序列号、地址、类、Outer、FName、vtable 与 Runtime epoch。命中时复核身份；过期和缺失路径重新枚举，支持对象数组数量不变的场景新增对象。初始化、读取器和回调变更使旧句柄失效。所有内存读取经过边界、页面与异常保护，或显式读取器回调。

原生派发在初始化的游戏线程执行，并要求 `Runtime::SetLifetimeGuard` 接入宿主实际的引擎 GC 排他或固定对象机制。未安装、GC 保护获取失败、对象身份失效、不可执行入口会中止调用；异常退出释放已获取的保护。回调宿主负责其对象生命周期与派发契约。

查找返回的裸指针是借用值。跨帧保存使用 `MakeObjectHandle` / `FindObjectHandle`，再次使用前调用 `ResolveObjectHandle`。序列号为零表示引擎尚未分配弱引用身份，此时不会生成持久句柄。普通互斥锁不能替代引擎 GC 保护。

保存的模块代码复验确认：对象项步长 24、每块 65,536 项、序列号位于对象项 `+0x10`，弱引用解析器使用 `0x10200000` 排除不可达/垃圾标志。版本配置和原始指令证据分别保存在 `profiles/nte-sdk-runtime.json` 与本地 `out/reports/runtime_identity_static_20261122.json`。

## 布局与字段

`UStruct +0x5C` 是 16 位 MinAlignment；旧实现读取 32 位，把相邻元数据合入数值。采集器现在使用配置明确的读取宽度，并保存完整类型头与属性目标槽证据。独立 Python 解码器检查对象身份、模块绑定、原始大小、字段偏移、元素大小、数组维度和 flags；不匹配的证据不能用于补齐语义。

`UClass::PropertiesSize` 表示字段跨度。蓝图字段跨度可以是 `0x32E`，MinAlignment 可以是 8；将基类声明为 `alignas(8)` 会使派生 C++ 大小取整为 `0x330`，成员结构体的显式对齐也会在 MSVC 中传播到类。声明统一使用紧凑字段视图，并断言原始 `sizeof`、`offsetof` 与视图 `alignof == 1`；反射 MinAlignment 单独记录在 `layout_audit.json`。这些类型用于反射字节存储与 ProcessEvent 参数传递。编译通过不等于全部运行时 ABI 已验证，`alignment_abi_verified_layouts` 保持 0。

目标引用只读取属性本身的目标槽，避免旧 `+0x20` 扫描混入相邻分配的指针；新增 `UserDefinedStruct` 识别与采集。142 个尚未解决字段主要为 41 个对象目标、84 个结构体字段和 17 个 FieldPath 字段，仍保留明确存储与缺项记录。

## 合并、网络与符号

`merge-sdk-captures` 接受按优先级排序的同版本快照，最后一份作为主快照。布局相同时补充缺少的目标语义；布局变化、语义冲突、未验证依赖分别列入 `merge_report.json`。本次补回 149 个登录、更新器和创建角色相关类型，未发现布局或语义冲突；这些记录不能替代相应场景的实际操作验收。

`--extra-network-capture` 对每份网络内存证据独立复验。失败场景中失效的类表被剔除并记录，合法缓存仍可参与比较。同一类和字段出现不同索引时，聚合查询保持不可用。另一份场景快照未提供额外有效 RPC 索引，293 个 RPC 与包级收发仍需补采，`PacketMappingVerified` 保持 false。

函数参数结构体名称固定为可读名称加完整反射路径 SHA-256 的 12 位后缀；不会随遍历顺序或新同名函数变化。`--symbol-baseline` 读取旧 `type_map.json`，生成 `symbol_migration.json` 和兼容别名。本次 47,421 个变化名称均生成兼容别名，覆盖原先会漂移的 72 个参数结构体名称。

```powershell
python tools/ebon_cli.py merge-sdk-captures `
  --capture out/reflection/startup_capture --scene startup `
  --capture out/reflection/gameplay_capture --scene gameplay `
  --out out/reflection/merged_sdk

python tools/ebon_cli.py generate-sdk `
  --dump out/reflection/merged_sdk `
  --network-capture out/reflection/gameplay_capture `
  --extra-network-capture out/reflection/another_gameplay_capture `
  --symbol-baseline out/sdk/CppSDK/type_map.json `
  --out out/sdk/CppSDK-merged
```

Windows 占用固定 SDK 目录时，发布器保留完整产物到 `CppSDK.ready_*`，`sdk_body_state.json` 与日志指向真实目录。本轮完整 SDK 为 `out/sdk/CppSDK.ready_20261122_181346_c9a35c7b`；原 `CppSDK` 目录由于占用仍是旧版。

验证记录与缺项保存在本地 `out/reports/sdk_lifecycle_publish_20261122.json`、`layout_audit.json`、`unresolved.json`、`symbol_migration.json` 和 `merge_report.json`。本轮不启动客户端，不声明场景换切或真实 RPC 调用已验证。

验证完成：180 项 Python/原生宿主测试通过；12 个原生目标重建成功；完整新版 SDK 使用 MSVC `/W4 /WX /bigobj` 编译、链接并运行 CompileCheck 成功。
