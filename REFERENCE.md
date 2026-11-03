# 工作区参考规则（Workspace Reference Rules）

> 本文件是本工作区（Sunrise 摄影模式开发，`E:\downloads\sunrise-src`）的常驻规则。

## 首要规则

**进行任何开发工作前，必须优先参考官方文档站：**

**https://projectsunrise.dev/docs/**

官方文档是 Sunrise / Sunrise Studio / Destiny 2 逆向结论的权威来源。任何设计、偏移、协议、类 ID、渲染管线问题的第一步都是查文档；文档没有答案时再做逆向/实验。上游源码仓库 [github.com/stanuwu/Sunrise](https://github.com/stanuwu/Sunrise) 与 [github.com/stanuwu/SunriseStudio](https://github.com/stanuwu/SunriseStudio) 的源码同样是重要参考。

## 本项目中已用到的高价值文档页

| 主题 | 页面 |
|---|---|
| 任务脚本 Lua API | /docs/mission-scripting/lua-api/ |
| Sunrise 架构与启动顺序 | /docs/sunrise/architecture/ |
| 设置、日志与游戏内菜单 | /docs/sunrise/settings-and-logs/ |
| 客户端钩子全表 | /docs/sunrise/client-hooks/ |
| Mod 系统（包补丁/合并） | /docs/sunrise/mods/ |
| 渲染管线（渲染阶段/部件可见性/布料） | /docs/destiny-2/rendering/ |
| Activity Host 协议（消息/槽类型） | /docs/destiny-2/activity-host-protocol/ |
| 包格式/标签与类 | /docs/destiny-2/packages/、/docs/destiny-2/tags-and-definitions/ |

## 本项目关键结论速记（从文档+逆向得到）

- **布料（职业物品裙摆）**：Havok 布料模拟独立驱动顶点，渲染时走实体模型的部件行（part row，类 `0x8080737E`，步长 0x20：材质@0、变体@4、拓扑@6、首索引@8、索引数@0xC、逻辑索引@0x14、**标志@0x18（u16）**、染料@0x1A、LOD@0x1B）；**标志 `0x40` = 游戏永不绘制该部件**（Rendering 文档 + Sunrise Studio `MeshData.HiddenPartFlag` 双重确认）。布料部件不走骨骼姿态生产函数（此前“跳过姿态生产”方案无效的原因）。
- 部件不绘制的另外两个开关：实体 enabled-part 位集中逻辑索引未置位；选中材质为 -1。
- 本地玩家 biped 由客户端本地构建，Activity Host 的脚本覆盖管不到玩家装备部件。
- 姿态生产函数返回 0 不删除网格，只会留下陈旧姿态数据（全跳过会闪烁错乱）——已弃用该方案。
- 摄影模式钩子经验：只钩函数真正入口；函数内部入口会被函数自身跳转命中导致寄存器错乱崩溃。

## 已弃用的方案

- 跳过姿态生产函数（对非蒙皮路径无效且造成闪烁）。
- 钩住函数“内部入口”的探针（崩溃，游戏无法加载）。
