# ai-passport-magpie

在 FoloToy AI Passport 上显示这台 Mac 上 Magpie 统计的 Token 用量。卡片上、下键切换今天、近 7 天和近 30 天，确定键回到今天。卡片显示上次同步时间和用量来源状态。

## Magpie 页内插件

无需连接卡片即可在 Magpie 里查看用量。在仓库根目录执行以下命令后，从 Magpie 侧栏的插件区打开「ai-passport-magpie」。

```bash
/Applications/magpie.app/Contents/MacOS/magpie plugin add "$PWD"
```

插件页通过 Magpie 提供的只读用量权限取数。Mac 桥接程序通过低功耗蓝牙（BLE）同步卡片。

## 统计口径

近 7 天和近 30 天包含今天，时间窗口由 Magpie 划分。总量按 Magpie 用量面板计算，包含未缓存输入、输出、缓存读取和缓存写入。插件页显示所选时段的四项明细。卡片底部固定显示今天的输入、输出及缓存合计。数据包括 Magpie 网关记录和它导入的 Agent 直连记录。

Mac 侧优先读取已运行的 Magpie 本地用量接口。接口不可用时，调用已安装的 Magpie CLI 导出统计。卡片通过加密的蓝牙连接接收聚合数字和更新时间，默认每 30 秒更新一次。

## Mac 端用法

需要 macOS 13 或更新版本、Magpie、Python 3.9 或更新版本、Xcode Command Line Tools，以及已刷入蓝牙版固件的 AI Passport。首次连接时，点亮卡片并长按确定键进入配对，再在本目录运行：

```bash
python3 -B bridge/passport_bridge.py
```

- 用 `--preview` 在终端查看真实用量 JSON。这个模式无需连接卡片。
- 用 `--scan` 列出附近的用量牌。如果同时发现多个，用 `--device UUID` 按扫描结果选定卡片。
- 用 `--once` 同步一次并等待设备确认。
- 桥接默认识别 `/Applications/magpie.app` 或 PATH 中的 `magpie`。可用 `--magpie /path/to/magpie` 指定另一个 CLI。

首次运行按 macOS 提示允许蓝牙访问，在系统配对对话框输入卡片显示的六位码。卡片出现 `BLE LIVE` 后，可拔掉 USB 数据线使用。USB 接口可以给卡片充电和刷机。

若 Magpie 数据暂时不可用，卡片保留上次数据并标记暂停。停止同步后，卡片约 90 秒后将上次数据标记为过期。保持桥接运行可在设备返回蓝牙范围时重新连接。空闲时卡片会先调暗，再关闭背光，蓝牙仍可收取数据。按任意功能键可唤醒屏幕。

## 固件构建与检查

激活 ESP-IDF 5.5.3 环境后运行：

```bash
./tools/validate.sh
```

验证通过后，合并固件位于 `build/FoloToy-AI-Passport-full.bin`。固件写入地址为 `0x0`，会替换卡片当前玩法，现有设置可能需要重新同步。出厂玩法可通过设备背面的专属二维码恢复。 [官方恢复指南](https://ai-passport.folotoy.cn/guides/restore-factory-firmware/)

通过 `npm test` 检查插件的时间窗口、汇总数值和刷新失败时的展示。

## 发布到自己的 GitHub

本仓库的 `upstream` 指向 FoloToy 固件源码。发布时，可把你自己的 GitHub 仓库设为 `origin`。Magpie 从仓库根目录的 `package.json` 识别插件，可通过发布后的 GitHub 地址安装。
