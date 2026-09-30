---
name: esp-idf-toolchain
description: '构建、烧录、诊断本项目 ESP-IDF esp32c6 固件的标准工作流。Use when: build 构建、flash 烧录、monitor 监视、idf.py 报错、编译错误 compile error、CMake 组件依赖、REQUIRES、managed_components、LVGL 编译错误、GCC -Werror 严格告警、sdkconfig / menuconfig 配置、分区表 partitions、固件大小 size、WiFi 配网 provisioning 固件调试。包含 VS Code ESP-IDF 扩展一键构建方法、手动环境变量后备方案、IDF 6.1 + GCC 15 + LVGL 9.6 已知兼容性坑与修复对照表。'
---

# ESP-IDF 工具链调用（ESP32-C6 / Waveshare Touch-AMOLED-2.16）

本项目工程名 `herdr_panel`，目标芯片 `esp32c6`，ESP-IDF **v6.1**，工具链
riscv32-esp-elf (GCC 15.2)，LVGL 9.6（托管组件）。

## 首选方式：VS Code ESP-IDF 扩展（不要手动配环境）

用户明确要求"点点就可以"——**一律使用 ESP-IDF 扩展命令**，不要在终端里
手动 `export` 环境变量或直接调 `idf.py`：

| 操作 | 扩展命令（`espIdfCommands` 工具） |
| --- | --- |
| 构建 | `build` |
| 烧录 | `flash`（先 `selectPort` 选串口） |
| 监视 | `monitor` 或 `buildFlashMonitor` |
| 清理 | `fullClean` |
| 目标芯片 | `setTarget`，target 参数 `esp32c6` |
| 菜单配置 | `menuconfig` |
| 体积分析 | `size` |

构建产物：`build/herdr_panel.bin`（app 分区 6M，16MB flash，分区表
[partitions.csv](../../partitions.csv)）。

## 构建失败诊断流程

1. 先用 `get_errors` 工具读编译诊断（language server，最快）。
2. `get_errors` 为空但仍失败 → 读 `build/log/idf_py_stdout_output_*` 与
   `idf_py_stderr_output_*`（每次构建生成一份）。
3. **终端输出被吞**（PowerShell 大量输出时常见）→ 把命令重定向到文件再搜：
   ```powershell
   cmd /c "ninja > C:\Users\tinys\ninja_out.txt 2>&1"
   Get-Content C:\Users\tinys\ninja_out.txt | Where-Object { $_ -match "error:|FAILED" }
   ```
   用完删除临时文件。
4. CMake **配置期**错误（`build_properties.temp.cmake`、`not scriptable`）与
   **编译期**错误分开看：配置期错误必须改组件 `CMakeLists.txt`。

## 手动环境变量（扩展不可用时的后备）

EIM 安装的环境（VS Code 扩展自动加载，手动跑才需要）：

| 变量 | 值 |
| --- | --- |
| `IDF_PATH` | `C:\esp\v6.1\esp-idf` |
| `IDF_TOOLS_PATH` | `C:\Espressif\tools` |
| `IDF_PYTHON_ENV_PATH` | `C:\Espressif\tools\python\v6.1\venv` |
| `ESP_ROM_ELF_DIR` | `C:\Espressif\tools\esp-rom-elfs\20241011/` |
| `PYTHONUTF8` | `1`（必须，否则中文输出报错） |
| `ESP_IDF_VERSION` / `IDF_VERSION` | `6.1.0`（idf_component_manager 需要） |

- PowerShell **执行策略禁止** `export.ps1` → 不要 source 它，直接用
  `C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`
  调 `C:\esp\v6.1\esp-idf\tools\idf.py`。
- `idf.py` 要求 PATH 含 cmake/ninja/工具链，版本以实际目录为准：
  `C:\Espressif\tools\cmake\4.0.3\bin`、`C:\Espressif\tools\ninja\1.12.1`、
  `C:\Espressif\tools\idf-exe\1.0.3`、
  `C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin`。

## 组件依赖规则（IDF 6.x）

- 组件注册表依赖写在 [main/idf_component.yml](../../main/idf_component.yml)；
  在其他组件 `REQUIRES` 里引用托管组件时用 **`namespace__name`** 双下划线名
  （如 `espressif__cjson`、`lvgl__lvgl`、`espressif__esp_lcd_sh8601`、
  `waveshare__esp_lcd_touch_cst9217`）。
- **公开头文件** `#include` 的组件必须放 `REQUIRES`；只在 `.c/.cpp` 里用的才可
  放 `PRIV_REQUIRES`，否则消费方编译报 `xxx.h: No such file or directory`。
- IDF 6.x 拆分了旧 `driver` 组件：用 `esp_driver_gpio` / `esp_driver_spi` /
  `esp_driver_i2c`，不再有总的 `driver`。
- IDF 6.1 移除了核心 `json` 组件 → 用组件注册表的 `espressif/cjson`
  （头文件仍是 `cJSON.h`）。
- **组件 CMakeLists 会被解析两遍**：第一遍是脚本模式（只提取依赖）。所以
  `add_compile_options` 会报 `not scriptable`——编译选项必须放在
  `idf_component_register()` **之后**用 `target_compile_options(${COMPONENT_LIB} PRIVATE ...)`。

## sdkconfig 管理

- 源头是 [sdkconfig.defaults](../../sdkconfig.defaults)；`sdkconfig` 是生成物，
  已被 gitignore。
- **改了 Kconfig 项名/删除项后**必须删掉 `sdkconfig` 再构建，否则旧值残留
  （如 `LV_MEM_SIZE_KILOBYTES` 换成 `LV_MEM_SIZE` 时踩过此坑）。
- 运行时可改的配置（WiFi、后端地址）走 NVS + 配网页/设置页，
  `sdkconfig` 里只放出厂默认值；编译期配置（动作文本、轮询间隔）留 Kconfig。

## 已知坑速查

编译错误 → 修复的完整对照表见
[references/troubleshooting.md](./references/troubleshooting.md)。高频三条：

1. GCC 15 `-Werror` 把所有告警升级为错误（`-Wmissing-field-initializers`、
   `-Wstringop-truncation`、`-Wformat-truncation`、`-Wrestrict`、`-Wnarrowing`）。
2. LVGL 9.6 大量弃用 API（`lv_obj_add_flag` 等）+ 废弃 Kconfig 项。
3. LVGL 默认主题给 `lv_obj` 容器加约 20px 内边距——绝对坐标布局的屏幕/底栏/
   列表行必须 `lv_obj_set_style_pad_all(x, 0, 0)`，否则元素越界。

## 与 Git 配合

- `.gitignore` 已排除 `build/`、`managed_components/`、`sdkconfig`、
  `dependencies.lock`、`refer/`（后者是嵌套 git 仓库的参考资料）。
- 构建通过后提交并推送 `git@github.com:ca1ic0/herdr-esp32-panel.git`（main）。
