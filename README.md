# vmrp-miyooflip - Mythroad/冒泡 mrp游戏模拟器

基于 [vmrp](https://github.com/vmrp/vmrp) 的 Mythroad（冒泡）平台模拟器，用于在开源掌机上运行早期国产功能手机的 MRP 游戏。已测试miyooflip、trimuibrick、miyoomini


## 依赖

| 组件 | 用途 |
|------|------|
| SDL2 | 窗口、渲染、输入 |
| SDL2_mixer | 音频播放 |
| dynarmic | ARM CPU 模拟（首选） |
| Unicorn Engine v1 | ARM CPU 模拟（备选） |
| Capstone | ARM 反汇编（仅 DEBUG 构建需要） |

**Unicorn Engine** 需要手动编译或使用预编译库（`lib/` 和 `aarch64_lib/` 目录已提供）。

## 构建

### x86_64 (Linux)

```bash
# Release 构建
make dynarmic

# Debug 构建（带 ARM 调试器支持）
make dynarmic DEBUG=1
```

产物生成在 `./bin/main`。

### ARM64 交叉编译

```bash
make -f Makefile.aarch64 dynarmic
```

产物生成在 `./bin/main_aarch64`。

### ARM32 交叉编译

```bash
make -f Makefile.arm32 arm32 -j4
```

产物生成在 `./bin/main_arm32`。

### 构建参数

| 参数 | 说明 |
|------|------|
| `DEBUG=1` | 启用调试模式（编译器符号 + Capstone 反汇编 + 指令追踪） |
| `NETWORK_SUPPORT` | 默认启用，网络 socket 支持 |

## 运行

从 `bin/` 目录运行（程序会从当前目录读取 `cfunction.ext`）：

```bash
cd bin
VMRP_CPU_BACKEND=dynarmic ./main <窗口宽> <窗口高> [mrp文件] [入口文件] [入口函数]
```

**参数说明：**
- `VMRP_CPU_BACKEND`: dynarmic（默认首选）、unicorn、native（arm32机器只能选择这个）
- `width` / `height`：窗口尺寸（必填）
- `mrp文件`：要启动的 .mrp 文件（默认 `dsm_gm.mrp`）
- `入口文件`：MRP 中的入口 Lua 脚本（默认 `start.mr`）
- `入口函数`：入口函数名（可选，默认 NULL）

**示例：**
```bash
# x64 启动默认游戏（mythroad文件夹下的dsm_gm.mrp，注意dsm_gm.mrp虽然在mythroad文件夹下，但是启动参数里mrp文件路径不要带上mythroad）
VMRP_CPU_BACKEND=dynarmic ./main 640 480

# trimui brick 启动指定 MRP 游戏（启动mythroad文件夹下的app240320/abc.mrp）
VMRP_CPU_BACKEND=dynarmic ./main_aarch64 1024 768 app240320/abc.mrp

# miyoo mini
VMRP_CPU_BACKEND=native ./main_arm32 640 480 app240320/app.mrp
```

## 按键映射

### 键盘默认映射

| 按键 | 功能 |
|------|------|
| W / ↑ | 上 |
| S / ↓ | 下 |
| A / ← | 左 |
| D / → | 右 |
| Q / [ | 左功能键 |
| E / ] | 右功能键 |
| Enter | 确认/OK |
| - | * 键 |
| = | # 键 |
| 0-9 | 数字键 0-9 |
| Tab | 接听键 |
| Esc | 挂机键/关机 |

### 手柄支持

自动检测手柄，方向键/摇杆控制方向，按键映射可通过 `keymap.cfg`（JSON 格式）自定义。
- **左摇杆按下**：方向键变为 2/4/6/8

### keymap.cfg 格式

```json
{
    "左键": "Y",
    "右键": "A",
    "OK": "X",
    "*": "SELECT",
    "#": "START",
    "0": "B",
    "1": "L",
    "3": "R",
    "7": "L2",
    "9": "R2"
}
```

键值对应手柄按钮：`Y`, `A`, `X`, `B`, `SELECT`, `START`, `L`, `R`, `L2`, `R2`。


## 许可

GPL v3.0 — 详见 LICENSE 文件。

基于 Unicorn Engine（GPL v2），SDL2（zlib License），Capstone（BSD）。
