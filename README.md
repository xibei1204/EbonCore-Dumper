# EbonCore-Dumper

Windows x64 / Unreal Engine 5 的只读反射采集、C++ SDK 生成与资源导出工具链。

> 当前版本绑定的样本为《异环》`HTGame.exe`。客户端更新后必须重新验证模块 SHA-256 与布局，不能直接复用旧偏移。本仓库不附带游戏文件、运行 dump、实际密钥或第三方解压 DLL。

## 能做什么

- 采集已加载 UObject、类型、属性、枚举、函数与 RPC / 复制元数据。
- 生成 C++ 声明、继承、布局断言、布尔位访问器和网络查询接口。
- 只读采集并逐字节重验 UE `LocalNetworkVersion`，SDK 用可选 UInt32 导出，未观测时保持 `nullopt`。
- 用 F10 重复导出表行、资产配置、场景反射值及对象引用。
- F10 优先生成 feature_exports/systematic_game_features.json 与 feature_table_rows.jsonl：实际 DA_SystematicGameFeatures、DataTables 引用、完整表行、菜单按钮绑定和非默认菜单组件实例。
- network_metadata.json 的 systematic_game_features_component 只接受运行时交叉验证的 FClassNetCache，记录目标函数的真实 FieldNetIndex、父类缓存、字段数量和索引边界；未采到时保持 not_observed，不会用 -1 或 RPCId 猜值。
- F10 生成战斗资产及蓝图依赖清单；export-battle-assets 按装载路径保全对应原始包文件，保留缺包和 IoStore 重建边界。详见 [菜单与战斗资源采集](docs/GAMEPLAY_CAPTURE.md)。
- F10 优先保存配置正文，再独立采集网络布局；角色配置、依赖与菜单分别分配名额。`audit-gameplay` 比较同版本多次采集，检查正文缺项并重新验证复制布局字节，详见 [采集优化](docs/GAMEPLAY_OPTIMIZATION.md)。
- `resources.jsonl` **只作索引**；正文保存在各分类 `data/*.jsonl` 独立文件中。
- 保全 loose / Pak / IoStore 资源，保留来源路径、状态与 SHA-256。
- 搜索 AES Pak 索引密钥，只接受完整索引 SHA-1 验证通过的候选；在本次采集的 Dumper 终端窗口内按 F9 触发（也可写 `aes_hunt.request`）。

这不是完整资源解包器或服务端实现：未加载对象不在内存采集范围，IoStore 原始 chunk 不等于重建的 `.uasset`；未解析字段、默认对象空值和部分表均保留明确标记。

## 环境

| 组件 | 要求 |
| --- | --- |
| 系统 | Windows 10 / 11 x64 |
| 编译器 | Visual Studio C++ x64 工具链，C++17 |
| Python | 3.11+；主流程及合成测试只需标准库 |
| 可选依赖 | `pycryptodome`、`capstone`，见 [requirements.txt](requirements.txt) |

## 快速开始

```powershell
git clone https://github.com/xibei1204/EbonCore-Dumper.git
cd EbonCore-Dumper
cmd /c build_native.bat
python -m unittest discover -s tests -p 'test_*.py' -v
```

构建脚本自动定位 Visual Studio；也可在 x64 Native Tools 终端执行，或用 `EBON_VCVARS` 指定 `vcvars64.bat`。原生产物写入 `build/`，不随源码仓库分发。

### 采集与 SDK

```powershell
# 先启动本工具，再按提示从自己的启动器启动目标客户端
.\run_inject.bat

# 已有完整反射快照：按实际目录修改路径
python tools/ebon_cli.py generate-sdk `
  --dump 'out/reflection/your_capture' --out 'out/sdk/your_sdk'
```

只有成功且通过布局校验的快照可用于生成 SDK。注入器的加载成功提示不能代替 `dump_state.json` 的采集状态。

需要同时确认 UE 握手版本时，给 `inject-and-dump` 加 `--wait-network-version`；也可对已有本次快照执行 `verify-network-version --capture <目录>`。具体版本绑定、代码与缓存证据见 [NetworkVersion 说明](docs/NETWORK_VERSION_2026-11-15.md)。

### 资源正文

等待日志显示 `F10 resource dump ready`，在目标游戏或对应 EbonCore 日志窗口前台按 F10；也可通过 `tools/request_resource_capture.ps1` 请求采集。

```text
out/reflection/your_capture/resources/res_1/
├─ resource_state.json
├─ resource_manifest.jsonl
├─ data/DataTable/Avatar/AvatarDataTable.jsonl
└─ gacha/
   ├─ resources.jsonl           # 索引，不包含正文
   └─ data/DataTable/Avatar/AvatarDataTable.jsonl      # 独立正文，按对象路径命名
```

正文按对象路径命名，`/Game/` 映射为 `data/` 下的内容路径；其他包保留 `/Script/`、`/Engine/` 等根名。子对象追加完整 outer 名称以避免冲突。索引 `file` 相对于 `res_1` 根目录。旧编号 dump 可在不覆盖原件的情况下重导为原名路径：

```powershell
python tools/ebon_cli.py export-resource-bodies `
  --capture 'out/reflection/your_capture/resources/res_1' `
  --out 'out/resources/your_bodies'
```

有效但未索引的已写出正文会标为部分资源；不补造缺失行、未解析字段或活动配置。

### 磁盘资源保全

```powershell
# 按实际客户端路径修改 --client；加密索引才需要 EBON_AES_KEY
python tools/ebon_cli.py dump-res `
  --client 'C:/path/to/client/Content' --out 'out/resources/your_bundle'
```

`dump-res` 与 `export-resource-bodies` 的退出码：`0` 无未解决项，`1` 已保全部分内容但仍有缺项，`2` 失败或没有可用产物。以对应状态文件为准。

## 项目结构

```text
native/          # C++ 采集器、注入器、AES 工具与合成自测
tools/ebon/      # Python 容器读取、SDK 生成、网络映射与正文导出
tools/*.ps1      # 资源 / 网络补采请求
profiles/        # 已确认样本版本的布局与入口配置，不含实际密钥
tests/           # Python 单元测试与 Windows 隔离宿主集成测试
docs/            # 详细用法、实现边界及历史验证记录
.github/         # Windows 构建与测试工作流
build_native.bat # 一次构建全部 10 个原生目标
```

`build/`、`out/`、`.work/`、个人指令文件和一次性探针只保留本地；密钥、凭据、客户端资产、dump 与日志均从版本控制排除。

## 文档与贡献

- [详细使用指南](docs/USAGE.md)：采集、SDK、AES、容器导出及参数。
- [文档索引](docs/README.md)：资源正文、反射、网络与历史验证记录。
- [贡献指南](CONTRIBUTING.md)：构建、测试、提交与敏感数据处理。
- [安全说明](SECURITY.md)：安全问题及密钥泄漏的报告方式。

CI 安装可选依赖并在合成宿主上构建、测试，不启动游戏、不验证真实客户端兼容性；本机客户端容器测试在无客户端的 CI 环境中明确跳过。历史文档中的本地 `out/` 证据路径不随公仓提供。

## 许可证与边界

Copyright (C) 2026 xibei1204 and contributors.

项目源码按 **GNU GPL v3.0（GPL-3.0-only）** 发布，完整条款见 [LICENSE](LICENSE)。第三方依赖及参考项目遵循各自许可，参见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

工具按“现状”提供，不作可用性、完整性或 ABI / 数据包兼容性保证。请自行确认目标软件、服务、数据及运行环境的授权；项目许可证不授予任何第三方资产或服务的访问权。
