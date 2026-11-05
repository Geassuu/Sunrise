# 工作区参考规则（Workspace Reference Rules）

> 本文件是本工作区（Sunrise 摄影模式开发，`E:\downloads\sunrise-src`）的常驻规则。

## 首要规则

**进行任何思考、设计或开发之前，必须先查阅本地镜像的官方文档（`docs/` 目录），其次才是网页：**

- 本地镜像：**`docs/<分区>/<页面>.md`**（本仓库内，含全部 90+ 官方文档页的完整正文）：
  - `docs/mission-scripting/` —— 任务脚本（Lua API、世界模型、SDK 视图、调试等 12 页）
  - `docs/sunrise/` —— Sunrise 本体（架构、设置日志、启动流程、存档、Activity Host、客户端钩子、Mod 系统 7 页）
  - `docs/sunrise-studio/` —— Sunrise Studio（界面、指南、构建、内部机制 50 页）
  - `docs/destiny-2/` —— Destiny 2 逆向（构建、包格式、投资数据、Activity Host 协议、渲染、特效、UI、音频、AI 17 页）
  - `docs/destiny-1/` —— Destiny 1 对照（7 页）
- 在线站点：**https://projectsunrise.dev/docs/**（与本地镜像同源，用于核对更新）

官方文档是 Sunrise / Sunrise Studio / Destiny 2 逆向结论的权威来源。任何设计、偏移、协议、类 ID、渲染管线问题的第一步都是查本地 `docs/` 对应页面；文档没有答案时再做逆向/实验。上游源码仓库 [github.com/stanuwu/Sunrise](https://github.com/stanuwu/Sunrise) 与 [github.com/stanuwu/SunriseStudio](https://github.com/stanuwu/SunriseStudio) 的源码同样是重要参考。

## 本项目关键结论速记（从文档+逆向得到）

- **布料（职业物品裙摆）**：Havok 布料模拟独立驱动顶点，渲染时走实体模型的部件行（part row，类 `0x8080737E`，步长 0x20：材质@0、变体@4、拓扑@6、首索引@8、索引数@0xC、逻辑索引@0x14、**标志@0x18（u16）**、染料@0x1A、LOD@0x1B）；**标志 `0x40` = 游戏永不绘制该部件**（Rendering 文档 + Sunrise Studio `MeshData.HiddenPartFlag` 双重确认）。布料部件不走骨骼姿态生产函数（此前“跳过姿态生产”方案无效的原因）。
- **布料绘制路径不读取部件标志**：运行时改内存、Studio Mod 补丁（822 个布料模型置 0x40）均实测无效——布料有自己的绘制提交路径，只看布料资产自己的列表。要隐藏布料只能钩布料绘制函数（高风险，未做）。
- 部件不绘制的另外两个开关：实体 enabled-part 位集中逻辑索引未置位；选中材质为 -1。
- 本地玩家 biped 由客户端本地构建，Activity Host 的脚本覆盖管不到玩家装备部件。
- 姿态生产函数返回 0 不删除网格，只会留下陈旧姿态数据（全跳过会闪烁错乱）——已弃用该方案。
- 摄影模式钩子经验：只钩函数真正入口；函数内部入口会被函数自身跳转命中导致寄存器错乱崩溃。

## 相机载体/身份重定向实验（已休眠，保留代码）

- 本地玩家身份句柄 = 受控句柄函数（`40 53 48 83 EC 20 ...`）经两层间接查表：`[表基址 + (中间值 & 0x1FFF) × 0x1E0 + 0x54]`；两个全局由函数内 rip 相对偏移（`fn+0x2E+0x01ADEB92` 步长 dword、`fn+0x35+0x01ADEB83` 基址 qword）解析，ASLR 无关。实测本地玩家槽 = 表首条目（索引 0）。
- SDK 小队生成（`activity_sdk_squads::place`）可用：取绑定场景（`scenario_squads`）中第一个可放置小队，计数向量长度须与成员数一致；`resolve` 需要恰好一条已认证活动链接（加载期要重试）。
- **实测结论：身份重定向会切换输入/物理的“玩家”（走到载体上），但游戏相机锚定在真实玩家角色上，不读身份句柄——重定向后相机冻结。** 要让相机骑载体需再逆向相机系统的角色指针（小队角色没有玩家相机装置，成功率低）。已回退为“隐形玩家身体承载相机”（骑身体方案）。
- 越界倒计时：游戏对本地玩家身体越界触发，骑身体方案飞出边界会倒计时死亡；未见客户端侧可关闭的开关。
- HUD 隐藏：活动 lifetime 状态 1（文档确认“HUD hidden”）；推送须走 `current_activity_link_view` + `request_activity_lifetime_override`（本地活动链接路径），SDK 视图路径在无区域选择时被拒（noActivityLink）。

## 已弃用的方案

- 跳过姿态生产函数（对非蒙皮路径无效且造成闪烁）。
- 钩住函数“内部入口”的探针（崩溃，游戏无法加载）。
- 身份重定向驱动相机（相机锚定真实玩家角色，冻结画面）。
- 部件标志 0x40 隐藏布料（运行时写内存 + Mod 补丁均无效——布料绘制不读该标志）。
