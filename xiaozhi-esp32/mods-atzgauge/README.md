# mods-atzgauge —— AtzGauge 改动套件

对 `78/xiaozhi-esp32` 的**全部**改动都装在这里，用来在官方升级后快速重打。

- 基线：官方 **v2.5.0**
- 改动规模：**2 个上游文件（3 处连续块）+ 15 个新增文件**
- 已实测：把本套件应用到纯净 v2.5.0，重建结果与当前工作树**逐字节一致**

## 官方升级了怎么办（4 步）

```powershell
# 1) 下载新版官方源码，解压成新目录（别覆盖旧树）
# 2) 重打改动（幂等；加 --dry-run 只看会做什么）
node D:\AtzGauge\xiaozhi-esp32\mods-atzgauge\apply-mods.mjs D:\path\to\new\xiaozhi\src
#    这一步会自动跑校验；也可以单独跑：
node D:\AtzGauge\xiaozhi-esp32\mods-atzgauge\verify-mods.mjs D:\path\to\new\xiaozhi\src
# 3) 编译
powershell -NoProfile -ExecutionPolicy Bypass -File D:\AtzGauge\tools\idf-run.ps1 `
    -Command "python scripts/build.py waveshare/esp32-s3-touch-lcd-1.85-atzgauge"
# 4) 烧录
powershell -NoProfile -ExecutionPolicy Bypass -File D:\AtzGauge\tools\idf-run.ps1 `
    -Command "cmd /c D:\AtzGauge\tools\flash-atzgauge.bat"
```

## 改了自己的代码之后

重新生成一次套件（会刷新 diff、new-files、blocks）：

```powershell
node D:\AtzGauge\tools\gen-mods-kit.mjs
```

需要一份纯净上游作为基线，默认 `D:\AtzGauge\upstream\xiaozhi-esp32-2.5.0`。
拉取地址：`https://codeload.github.com/78/xiaozhi-esp32/tar.gz/refs/tags/v2.5.0`

## 目录

| 路径 | 用途 |
|---|---|
| `MODIFICATIONS.md` | **★ 先看这个**：逐条台账 + 升级流程 + 注意事项 |
| `manifest.json` | 机器可读清单（新增文件 / 锚点 / 抽查项） |
| `modified-upstream.patch` | 上游 2 个文件的真实 diff（3 hunk） |
| `new-files/` | 15 个新增文件快照 |
| `blocks/` | 3 处上游改动原文（apply 脚本用） |
| `apply-mods.mjs` / `verify-mods.mjs` | 一键重打 / 只读校验 |
