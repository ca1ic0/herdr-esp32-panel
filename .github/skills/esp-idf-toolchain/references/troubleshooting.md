# 编译错误速查表（IDF 6.1 + GCC 15 + LVGL 9.6）

本项目实际踩过的坑及修复方式。报错信息列是搜索关键词。

## GCC 15 `-Werror` 严格告警

| 报错关键词 | 根因 | 修复 |
| --- | --- | --- |
| `missing initializer for member 'xxx'` | 结构体/宏只初始化了部分字段，GCC 15 升级为错误 | 优先补齐字段初始化（如 `esp_lcd_touch_config_t` 的 `process_coordinates`）；托管组件头里的宏无法改 → 组件级 `target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)`（放 `idf_component_register` 之后） |
| `'strncpy' output may be truncated` / `-Wstringop-truncation` | `strncpy` 固定长度复制到同长缓冲 | 换 `memcpy(dst, src, strnlen(src, sizeof dst))` |
| `'%s' directive output may be truncated` / `-Wformat-truncation` | `snprintf` 从大缓冲 `%s` 到小缓冲 | 加精度限定 `%.32s` / `%.63s` |
| `'snprintf' argument may overlap destination` / `-Wrestrict` | 同一结构体字段间复制被误判重叠 | 换 `memmove` |
| `narrowing conversion of ... from 'int' to 'uint16_t'` | C++ 列表初始化窄化 | 显式强转 `(uint16_t)` |
| `conflicting types for 'cookie_io...'` / `unknown type name '__FILE'` | 工具链 libc 头与 `esp_libc` 兼容层不匹配（多为编译选项注入位置错误的连带错误） | 检查组件 CMakeLists，编译选项用 `target_compile_options` 且放注册之后 |
| `'XXX' defined but not used` | 未使用的 `TAG` 等静态变量 | 删除或 `(void)` 引用 |

## CMake / 构建系统

| 报错关键词 | 根因 | 修复 |
| --- | --- | --- |
| `add_compile_options command is not scriptable` | 组件 CMakeLists 在依赖扫描期以脚本模式执行 | 把该命令移到 `idf_component_register()` 之后，或改用 `target_compile_options` |
| `Failed to resolve component 'json': unknown name` | IDF 6.1 移除了核心 `json` 组件 | `main/idf_component.yml` 加 `espressif/cjson: ^1.7.18`，REQUIRES 用 `espressif__cjson` |
| `driver/xxx.h: No such file or directory`（gpio/spi/i2c） | IDF 6.x 拆分旧 `driver` 组件 | REQUIRES 改 `esp_driver_gpio` / `esp_driver_spi` / `esp_driver_i2c` |
| `xxx.h: No such file or directory`（头在别的组件） | 组件头链公开暴露但只写了 `PRIV_REQUIRES` | 提升为 `REQUIRES`（公开） |
| `The CMAKE_C_COMPILER ... was not found in the PATH` | 手动构建时 PATH 缺工具链 | PATH 加 `C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin`（版本号以目录实况为准） |
| `Syntax Warning in cmake code ... IDF_VER="v6.1"` | IDF 自身属性序列化的引号（dev 警告） | 忽略即可，不阻塞构建 |

## LVGL 9.6

| 报错关键词 | 根因 | 修复 |
| --- | --- | --- |
| `LV_MEM_SIZE_KILOBYTES is deprecated`（`-Werror=cpp` 变错误） | 旧 Kconfig 项触发 `#warning` | `sdkconfig.defaults` 改 `CONFIG_LV_MEM_SIZE=65536`（字节），并**删除 sdkconfig 重新生成** |
| `'lv_obj_add_flag' is deprecated` / `lv_obj_remove_flag` / `lv_obj_clear_flag` | v9.6 弃用批量 flag API | 改 `lv_obj_set_hidden()` / `lv_obj_set_scrollable()` / `lv_obj_set_clickable()` |
| `Both LV_MEM_SIZE and LV_MEM_SIZE_KILOBYTES are defined` | 旧项残留 sdkconfig | 删 sdkconfig 再构建 |

## LVGL 运行期（编译通过但显示不对）

| 现象 | 根因 | 修复 |
| --- | --- | --- |
| 元素超出屏幕边界 | 默认主题给 `lv_obj` 加约 20px 内边距（`PAD_DEF`），绝对坐标子元素相对内容区定位被整体推移 | 屏幕/底栏/列表行等容器 `lv_obj_set_style_pad_all(x, 0, 0)`（必要时 `pad_row`/`pad_column` 也清零） |
| 长文本撑破容器 | 自动宽度 label 无截断 | `lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT)` + `lv_obj_set_width()` |

## 启动崩溃（编译通过但黑屏/重启循环）

| 现象 | 根因 | 修复 |
| --- | --- | --- |
| 配网模式启动即重启，无二维码 | `esp_netif_create_default_wifi_ap()` 在 `esp_netif_init()` / `esp_event_loop_create_default()` 之前调用 | 两个初始化移到任何 netif 创建之前（AP 和 STA 两条路径都要） |
| `ESP_ERROR_CHECK` abort | 任何初始化顺序错误 | 看串口 monitor 的 abort 栈；init 顺序：NVS → netif → event loop → wifi_init → netif_create → set_config → start |

## 手动构建时的环境问题

| 现象 | 根因 | 修复 |
| --- | --- | --- |
| `禁止运行脚本`（无法 source export.ps1） | PowerShell 执行策略 | 不 source，直接用 venv python 调 `tools/idf.py` |
| `Your environment is not configured to handle Unicode` | 缺 UTF-8 模式 | 设 `PYTHONUTF8=1` |
| `expected string or bytes-like object, got 'NoneType'`（component_manager） | 缺 `ESP_IDF_VERSION` | 设 `ESP_IDF_VERSION=6.1.0` |
| 终端"Command produced no output" | PowerShell 吞掉大量 stdout | `cmd /c "cmd > out.txt 2>&1"` 重定向到文件再读 |
