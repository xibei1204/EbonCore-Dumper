# 独立资源正文文件

> `resources.jsonl` 保持索引格式；正文保存在独立文件。此修复不恢复未加载资源，不把 CDO 空值或未解析字段改成真实配置。

## 范围与根因

- BF-RES-004：分类目录只有索引，正文仅在根目录 `data/`，不方便直接从分类查看；超时表在 `index()` 前退出，已有正文可能未登记。
- E-001：[V] `native/dumper/ebon_resource_dump.hpp` 原 `index()` 将同一条元数据写入分类索引；正文已由 table/stringTable/objectValues/DataAsset 路径写入根 `data/`。
- E-002：[V] `out/reflection/network_layouts_20261115_004927/resources/res_1/resource_manifest.jsonl` 有 343 个资源，`data/` 有 344 份正文；`data/table_86710.jsonl` 是未索引的 `DT_FT_Armor`。原状态为 `failed` / `resource capture deadline exceeded`。
- E-003：[V] 预改默认 `build/` 的资源 Python 回归失败：旧自测未生成 `scene/data/object_203.jsonl`。这属于已有二进制/源码不一致，不能把旧构建当作本次验证通过。

## 改动与路径

- `native/dumper/ebon_resource_dump.hpp`：完成并关闭原正文后，复制到各分类 `data/`；索引 `file` 指向分类副本，`source_file` 指向根正文。保留根清单格式。全部副本和索引计入字节预算，拒绝覆盖已有副本。
- `native/sdk/EbonResourceSelfTest.cpp`、`tests/test_resource_dump.py`：检查表、资产、StringTable、地图和脚本正文的字节一致性、索引不嵌入值、字节预算及防覆盖。
- `tools/ebon/resource_bodies.py`、`tools/ebon_cli.py`：`export-resource-bodies` 只读取已有 F10 正文；校验 JSONL、身份和路径，独占发布独立副本。有效未索引正文归入 `other_assets` 并显式标记部分资源。
- `tests/test_resource_bodies.py`：覆盖独立副本、原件不变、部分表恢复、身份冲突、路径逃逸、坏 JSON、预算、状态竞态和退出码。
- `README.md`：操作说明。

P-001（callflow）：F10 → 读取并完成根正文 → 预算检查 → 分类正文副本 → 元数据索引。

P-002（callflow）：旧 F10 根清单 / 未索引 `data/*.jsonl` → JSONL 与身份校验 → 根正文和分类副本 → 索引 → 独立导出状态。源采集失败不改写成成功采集。

## 使用

所有 `file` 路径相对于本次采集/导出根目录，而非索引所在目录。

```text
res_N/
├─ resource_manifest.jsonl
├─ data/asset_75973.jsonl
└─ gacha/
   ├─ resources.jsonl
   └─ data/asset_75973.jsonl
```

旧 dump 的转换命令见 [详细使用指南](USAGE.md#6-f10-导出资源配置值)。转换输出用 `resource_bodies_state.json` 区分“正文导出结果”和“原始资源采集结果”，不能当作重新采集成功的 F10 快照。

## 验证与回退

- Python 正文导出单元测试首轮：[V] 9 项通过；新增未解析统计后的最终验证另行记录。
- 首轮原生 10 个目标构建及独立资源自测：[V] 通过；`out/verification/resource-bodies-build.log`、`resource-bodies-native.log`。
- 首轮全量回归：[V] 88 项运行、1 项错误，`out/verification/resource-bodies-python-tests.log`。原因是新增防覆盖用例故意放入普通文本但用了 `.jsonl` 后缀，全输出 JSON 检查正确拒绝它；已将该占位样本改成有效 JSON，仍检查原字节不被覆盖。不是资源正文损坏，也不将这一轮记为通过。
- 旧 dump 导出：[V] `out/resources/bodies_network_layouts_20261115_004927/resource_bodies_state.json` 记录 344 个资源、817 个独立正文文件、1 个恢复的部分资源；原采集失败和 194,051 个原未解析项继续保留。全量副本哈希复核待追加。
- 最终结果见下表；首轮失败日志和原生样本保留，不清理、不覆盖。
- 开源工具选择：沿用 Python 标准库 `json` / `hashlib` 及项目现有路径校验、独占发布函数；不新增 JSON 解析器或资源反编译依赖。
- 审查：当前会话自审；没有多人签名。
- Git 快照：未执行，用户未要求提交；已有源码均未跟踪，不重置仓库。
- 预改备份：`.work/resource-bodies-baseline-20261115-011013/`，含源码、测试、README 和原默认二进制的副本及 SHA-256。
- 回退：只恢复该目录中本次修改文件；新增 `resource_bodies.py`、对应测试和本文档可单独停用。不要重置其他文件，也不要删除原始采集或新导出正文。
- [T] 未执行游戏进程内重新采集：运行中的进程仍可能持有旧 DLL；新正文输出需重新加载新 DLL 后验收。未修改 180 秒、值数量或文本/容器解码边界。

## 最终实测记录

| 证据 | 验证项 | 结果与路径 |
| --- | --- | --- |
| E-004 | 最终 10 个 MSVC x64 / C++17 目标 | [V] 构建通过，`out/verification/resource-bodies-final-build.log`；产物在 `build/candidate-resource-bodies/`。 |
| E-005 | 全量 Python 回归 | [V] 88 项通过，24.456 秒，`out/verification/resource-bodies-final-python-tests.log`。 |
| E-006 | 原生资源自测及全部输出结构检查 | [V] 206 个 JSON/JSONL 记录解析通过；分类正文与根正文逐字节相同，索引不嵌入值，`out/verification/resource-bodies-final-native/`、`resource-bodies-final-artifacts.json`。 |
| E-007 | 已有 dump 的 817 个正文文件 | [V] 全部 SHA-256 与来源一致，164,284,570 字节；根清单与分类索引都只有元数据；`out/verification/resource-bodies-existing-verified.json`。核验期间来源文件哈希不变。 |
| E-008 | 恢复部分表 | [V] `DT_FT_Armor` 已写出的 12,257 行保全；正文 `out/resources/bodies_network_layouts_20261115_004927/other_assets/data/table_86710.jsonl`，索引标记 `partial=true`。不宣称表完整。 |
| E-009 | 默认产物更新 | [V] `build/EbonDumper.dll`、`EbonDumper.exe`、`EbonResourceSelfTest.exe` 更新前与预改备份哈希相同，更新后与测试候选哈希相同；`out/verification/resource-bodies-default-artifacts.json`。其他默认目标不替换。 |
| E-010 | 默认路径复测 | [V] 26 项资源测试、4 项 DLL 隔离宿主测试通过；`out/verification/resource-bodies-default-resource-tests.log`、`resource-bodies-default-dll-tests.log`。不等于实机 F10 验收。 |

正文目录：`out/resources/bodies_network_layouts_20261115_004927/`。原始 dump 保留。默认 DLL 已更新，但需要重启客户端并重新加载才会影响后续 F10。

本次交付边界：独立正文组织、旧正文保全和自动验证已覆盖；默认对象空配置、未解析文本/容器字段以及原采集超时仍保留原始状态，不将“正文已导出”冒充“游戏配置全量恢复”。等待使用者验收，不创建 Git 提交，也不自动清理产物。
