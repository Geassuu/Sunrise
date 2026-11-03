# 摄影模式（Photo Mode）

在 Sunrise 游戏内菜单（游戏中按 Insert 呼出）新增 **Photo Mode** 页面，提供摄影模式：

- **自由移动摄像机**：开启后按住移动键（WASD/前进后退左右 + 跳跃/下蹲上升下降）以任意速度自由飞行，无视碰撞，悬停稳定；
- **隐藏玩家角色**：摄影模式下跳过本地玩家的蒙皮绘制（手臂、武器、人物本体），画面只剩场景；
- **照片保存到指定位置**：按下拍照键（或菜单里的 Take photo 按钮）把当前画面存为 JPEG，保存到可配置的文件夹，屏幕会提示文件名。

菜单页面：Movement 下方新增 **Photo Mode** 页，包含开关、开关快捷键、飞行速度、隐藏玩家开关、拍照键、保存文件夹（文本框 + Default folder + Open folder + Take photo 按钮）。改动即时写入 `bin/x64/Sunrise/photo.json`。

## 前置条件

与 Movement 页的 Fly/Noclip 一样，摄影模式属于作弊工具，需要在 `bin/x64/Sunrise/settings.json` 中开启：

```json
"client": {
  "cheats": true
}
```

开启后该会话进度不落盘（Sunrise 的作弊会话机制，与 Fly/Noclip 相同）。改完重启游戏。

## photo.json

首次在菜单里改动任意选项后自动生成于 `bin/x64/Sunrise/photo.json`：

```json
{
  "enabled": false,
  "toggle_key": 0,
  "shutter_key": 0,
  "speed": 10.0,
  "hide_player": true,
  "output_path": ""
}
```

- `toggle_key` / `shutter_key`：Windows 虚拟键码，0 表示未绑定（在菜单里点按钮后按任意键绑定，Esc 清除）；
- `speed`：飞行速度（世界单位/秒），范围 0.5–100；
- `output_path`：照片文件夹（UTF-8，支持中文）。留空表示默认 `<游戏目录>\bin\x64\Sunrise\photos\`，首次拍照自动创建。

## 构建

源码在 `Sunrise/src/` 下，新增文件已登记到 `Sunrise/Sunrise.vcxproj`，并按项目现有风格实现。

### 方式一：GitHub Actions（推荐，本机无需装编译器）

1. 把本仓库 fork 到你的 GitHub 账号，推送 `photo-mode` 分支；
2. 仓库 Actions 页 → Build → Run workflow（master 分支推送也会自动构建）；
3. 构建完成后在对应运行页下载 `Sunrise-Release-x64` 工件，取其中的 `steam_api64.dll`。

### 方式二：本机构建（Visual Studio 2026）

1. 安装 Visual Studio 2026，勾选「使用 C++ 的桌面开发」工作负载，确保 v145 工具集与 10.0.26100 Windows SDK；
2. 开发者 PowerShell：
   ```powershell
   msbuild Sunrise.sln /m /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64
   ```
3. 产物在 `build/x64/Release/steam_api64.dll`。

## 安装

1. 备份 `bin/x64/steam_api64.dll`（如 `steam_api64.dll.bak`）；
2. 用新编译的 `steam_api64.dll` 替换它；
3. 启动游戏 → Insert 打开菜单 → 左侧 **Photo Mode** 页。

## 已知限制与风险

- **隐藏玩家**通过跳过本地玩家的蒙皮姿态生产（与 Sunrise Studio 姿态观察者挂钩同一函数）实现：对属于本地玩家对象的绘制直接返回“未产出”。该返回值语义来自对游戏内调用约定的逆向结论，若某次游戏更新改变了契约，表现可能是角色模型异常而非隐藏——此时在 Photo Mode 页关闭 Hide player 即可恢复，无需重新编译。
- 截图从交换链后台缓冲拷贝并转成 8 位 RGB 后编码为 JPEG（质量 95）。支持 R8G8B8A8 / B8G8R8A8 / B8G8R8X8 / R10G10B10A2_UNORM / R16G16B16A16_FLOAT 五种缓冲格式，其他格式会记录 `ev=photo stage=capture result=fail reason=format`。
- 拍照时菜单画面不会出现在照片里（截图先于 ImGui 覆盖层绘制）。
- 摄影模式与 Fly 同时开启时，摄影模式拥有移动控制权。
