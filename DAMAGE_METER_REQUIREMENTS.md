# 组队伤害统计开发需求文档

## 1. 目标与范围

本需求仅实现“组队伤害统计”功能，不包含“单人按技能统计”。

功能目标：

- 当角色处于组队状态时，客户端显示当前队伍成员在当前战斗中的累计伤害。
- 客户端面板风格尽量贴近现有任务栏区域，显示为轻量级叠加组件。
- 统计结果以服务端为唯一可信来源，客户端只负责接收、缓存、排序和展示。

不在本期范围内：

- 单人按技能统计
- 跨地图、跨频道、跨实例统计
- 历史战斗查询、导出、排行持久化

## 2. 服务端开发需求

### 2.1 统计口径

- 统计对象：同一地图实例内、同一队伍的角色。
- 统计维度：`characterId -> totalDamage`。
- 统计来源：所有玩家实际生效的攻击结算。
- 累加规则：按最终实际扣除目标 HP 的伤害值累加，多段攻击逐段累计。
- 不计入：Miss、0 伤害、环境伤害、GM 指令伤害、非玩家来源伤害。

### 2.2 会话生命周期

服务端维护 `damageSessionId`，以下情况必须重置统计：

- 进入地图或切换频道
- 队伍成员变化
- 队伍解散
- 玩家离开当前地图实例
- Boss 或主要战斗目标死亡后结算完成

建议在重置前先推送一次最终快照，随后下发新会话或隐藏指令。

### 2.3 推送协议

建议预留自定义 Opcode：

- `0x1001` `DAMAGE_METER_SYNC`：服务端 -> 客户端

包结构建议：

```text
u16 opcode
u32 sessionId
u8  mode            // 0=隐藏, 1=组队统计
u8  reason          // 0=周期刷新, 1=重置, 2=战斗结束
u8  entryCount
repeat entryCount:
  u32 characterId
  string name
  u32 damageLow
  u32 damageHigh
```

说明：

- 总伤害使用 64 位整数，客户端通过 `damageLow + damageHigh` 还原。
- `name` 使用服务端现有角色名编码方式。
- `mode=0` 时客户端应清空并隐藏统计面板。

### 2.4 推送策略

- 不要按每次命中实时发包。
- 周期推送建议为 `500ms` 到 `1000ms` 一次完整快照。
- 队伍变更、地图切换、Boss 死亡、统计重置时立即推送。
- 客户端以最后一次快照为准，不做增量修正。

### 2.5 服务端实现要求

- 在近战、远程、魔法、召唤、DOT 等所有玩家伤害结算入口统一接入统计逻辑。
- 聚合 Key 建议包含：`channelId + mapId + instanceId + partyId + damageSessionId`。
- 仅统计当前地图实例中实际在场的队伍成员。
- 服务端必须是唯一可信统计源，客户端不得上传累计伤害值。

## 3. 客户端开发需求

### 3.1 本项目需要修改的内容

新增模块：

- `ezorsia/DamageMeter.h`
- `ezorsia/DamageMeter.cpp`

需要修改的现有文件：

- `ezorsia/dllmain.cpp`
- `ezorsia/config.ini`
- `ezorsia/ezorsia.vcxproj`
- `ezorsia/ezorsia.vcxproj.filters`

### 3.2 客户端职责

- Hook 当前收包流程，解析 `DAMAGE_METER_SYNC`。
- 缓存当前 `sessionId` 的完整快照。
- 在本地按总伤害降序排序。
- 计算每个成员的伤害占比。
- 在地图切换、隐藏指令或新会话时清空本地显示状态。
- 持续刷新 UI，但不自行推导或修正服务端统计值。

### 3.3 UI 显示要求

- 显示位置：优先放在任务栏上方或邻近区域，使用轻量 overlay 实现。
- 显示内容：角色名、累计伤害、占比。
- 显示数量：默认最多显示前 `6` 名，可配置。
- 刷新方式：跟随客户端更新循环刷新。
- 无组队或 `mode=0` 时隐藏面板。

可复用实现参考：

- 叠加显示和生命周期：`ezorsia/BossHP.cpp`
- 自定义收包 Hook：`ezorsia/HpMpAlert.cpp`
- 状态栏区域定位：`ezorsia/Client.cpp`、`ezorsia/codecaves.h`

### 3.4 配置项

在 `ezorsia/config.ini` 中新增可选项：

```ini
[optional]
enableDamageMeter=true
damageMeterMaxRows=6
damageMeterOffsetX=0
damageMeterOffsetY=0
```

要求：

- 功能可关闭。
- 坐标偏移用于不同分辨率微调。

## 4. 交互流程

1. 玩家进入地图并处于组队状态。
2. 服务端开始建立或恢复当前队伍的伤害统计会话。
3. 服务端按周期推送完整快照给客户端。
4. 客户端解析快照，覆盖本地缓存并刷新显示。
5. 当队伍变化、地图变化或战斗结束时，服务端下发重置或隐藏状态。
6. 客户端清空本地数据并隐藏或重建面板。

## 5. 验收标准

- 两名及以上队员攻击同一 Boss 时，客户端能稳定显示每名成员累计伤害。
- 排序、总伤害和占比与服务端快照一致。
- 地图切换、队伍变更、Boss 死亡后统计能正确清空或重置。
- 长时间战斗下数值不溢出。
- 周期推送不会明显造成客户端卡顿或网络抖动。
- 未组队时客户端不显示伤害统计面板。


# 编译
按这套流程走，目标就是在 Windows 上编出 out/Release/ijl15.dll。

  准备环境

  1. 你需要一台 Windows 机器或虚拟机。
  2. 安装 Visual Studio 2019。
  3. 安装时勾选 使用 C++ 的桌面开发。
  4. 在“单个组件”里确认这几个已安装：
      - MSVC v142 - VS 2019 C++ x64/x86 生成工具
      - Windows 10 SDK
      - C++ CMake tools for Windows 可以不装，不影响这个项目

  这个仓库测试环境写的是 VS 2019 + SDK 10 + v142，见 README.md:11。工程里也明确指定了 PlatformToolset=v142，见 ezorsia/ezorsia.vcxproj:38。

  编译 DLL

  1. 把仓库拷到 Windows，比如 D:\BeiDou-ijl15。
  2. 双击打开 ezorsia.sln:1。
  3. 在 VS 顶部把配置切到：
      - Release
      - x86
  4. 菜单里点“生成” -> “生成解决方案”。

  这里必须用 x86，因为解决方案把 Release|x86 映射到了工程的 Release|Win32，见 ezorsia.sln:22。工程类型本身就是 DynamicLibrary，也就是 DLL，见 ezorsia/
  ezorsia.vcxproj:35。

  成功后输出位置是：

  out/Release/ijl15.dll

  对应配置见 ezorsia/ezorsia.vcxproj:90 和 ezorsia/ezorsia.vcxproj:91。

  命令行编译

  如果你不用 VS 界面，可以打开 x86 Native Tools Command Prompt for VS 2019，进入项目目录后执行：

  msbuild ezorsia.sln /p:Configuration=Release /p:Platform=x86

  部署到客户端

  1. 先把客户端原来的 ijl15.dll 改名成 2ijl15.dll。
  2. 把新编出来的 out/Release/ijl15.dll 复制到客户端目录。
  3. 再把配置文件也放进去。仓库里的配置文件实际在 ezorsia/config.ini:1。

  常见报错

  - 提示找不到 v142：说明没装 VS2019 工具集。
  - 提示找不到 Windows SDK：补装 Windows 10 SDK。
  - 生成成了 x64：切回 Release + x86，不要用 x64。
  - 链接 detours.lib 失败：确认仓库里的 detours/detours.lib 存在，这个项目会直接链接它，见 ezorsia/ezorsia.vcxproj:148 和 ezorsia/ezorsia.vcxproj:149。