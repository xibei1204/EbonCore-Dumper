# EbonCore res 扩展验证报告

## 范围

用户目标：扩展 Dumper 的地图、Lua 及私服研究用资源导出。实现范围与待验证清单见 [res 扩展记录](RES_EXPANSION_2026-11-15.md)。输入只读、输出分离；没有改动官方服务端、没有执行真实游戏注入。

## Evidence → Finding → Path

| Finding | 证据 | 结论 / 位置 | 状态 |
| --- | --- | --- | --- |
| F-001（n/a_re） | E-001、E-003、E-010 | F10 增加 World/Level/Actor/组件、可反射坐标、游戏 UObject/CDO、脚本资产与引用边；`native/dumper/ebon_resource_dump.hpp` | [V] 合成验证；[T] 实机 |
| F-002（n/a_re） | E-006、E-011、E-012 | `dump-res` 保全 loose/Pak/IoStore，多来源独立路径、SHA-256、分类和预算；`tools/ebon/resource_bundle.py` | [V] 文件保全与合成容器验证 |
| F-003（n/a_re） | E-005、E-007、E-010、E-012 | F10 干净清单误拒绝及状态删除共享竞态已覆盖回归；`native/dumper/EbonDumper.cpp` | [V] 校验/真实文件共享拒绝测试 |
| F-004（n/a_re） | E-008 | 本机 `.hpw` 有混淆媒体头线索，不是取得真实 Lua 的证据 | [H] 媒体推断；[T] 全文件验证 |
| F-005（n/a_re） | E-011、E-012、G-001 | 可显式接入可信本地 Oodle DLL；不自动下载，加载前可核对哈希 | [V] 适配/模拟回调；[T] 实际 Oodle |

证据 ID、来源文件及时间线均在 [主记录](RES_EXPANSION_2026-11-15.md)。P-RES-001：CLI → 来源索引 → 有界只读提取 → 独占文件发布 → 字节哈希与分类 → 终态；F10 → 新反射 → 表/资产优先 → 场景/游戏对象 → 引用索引。

## 复现

```powershell
.\build_native.bat build\candidate-res-expanded-final
$env:EBON_TEST_BUILD_DIR = (Resolve-Path 'build\candidate-res-expanded-final').Path
python -m unittest discover -s tests -p 'test_*.py' -v
```

本轮最后运行：10 个原生目标构建通过、79 项全量回归通过；针对性 22 项通过。失败历史保留：此前 AES 宿主出现一次超时，复测通过但根因尚未定位。实机/密钥/Oodle/私服三类验收不可由自动测试替代。

新版 DLL：`build/candidate-res-expanded-final/EbonDumper.dll`。操作说明、预算、AES 环境变量和输出判读见 [详细使用指南](USAGE.md#8-批量-dump-地图lua热更与私服研究资源)。

## 边界

- IoStore 仅保留原始 chunk，不重建可直接加载的 cooked package。
- Lua 源文件/字节码保全能力已测；本机实际 Lua 尚未命中，VM dump/反编译未实现。
- 场景值仅覆盖当前已加载对象；不证明全部 World Partition、导航/碰撞与资产依赖闭包。
- 客户端没有的服务端数据库和权威逻辑无法由资源导出凭空生成。
- 状态文件明确 `server_resources_complete=false`，尚不宣称私服数据齐全。

审查状态：当前会话自审与工具验证，非多人签名；等待用户实机验收。Git 提交与自动清理均未执行。
