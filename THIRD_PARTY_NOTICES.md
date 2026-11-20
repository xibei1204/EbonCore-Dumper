# 第三方依赖与参考

项目源码采用 GPL-3.0；以下第三方内容保留各自许可。此列表不授予游戏资产、客户端二进制或第三方服务的使用权。

| 项目 | 用途 | 分发方式 / 许可 |
| --- | --- | --- |
| [CUE4Parse](https://github.com/FabianFG/CUE4Parse) | Pak / IoStore 格式阅读参考；来源注释见 `tools/ebon/containers.py` | Apache-2.0；不捆绑其代码或二进制。 |
| [PyCryptodome](https://github.com/Legrandin/pycryptodome) | 可选 AES 解密依赖 | 独立安装；遵循其 BSD-2-Clause 与公有领域组成部分的许可。 |
| [Capstone](https://github.com/capstone-engine/capstone) | 可选 x64 指令特征核对 | 独立安装；BSD-3-Clause，组件许可见上游。 |
| [pefile](https://github.com/erocarrera/pefile) | 可选 NetworkVersion PE 布局 / 代码映射核对 | 独立安装；MIT；不执行客户端代码。 |
| [actions/checkout](https://github.com/actions/checkout)、[actions/setup-python](https://github.com/actions/setup-python) | GitHub Actions CI | MIT；按固定提交引用，不捆绑源码。 |

Oodle 解压器不随项目提供；使用者需自行提供可信且获得许可的本地 DLL。Windows / Visual Studio / Unreal Engine 以及目标客户端遵循各自条款。
