# 开机白屏诊断

先删除项目根目录里的生成文件 `sdkconfig`，让 ESP-IDF 从更新后的 `sdkconfig.defaults` 重新生成配置；现有 `sdkconfig` 不会自动继承新的 LVGL 内存大小。这一步不会清除设备上的配网凭据。然后用 ESP-IDF 扩展的 **Build, Flash and Monitor** 烧录当前固件并查看串口。请保留从复位开始的完整日志；只看最后一行通常无法区分持续白屏与反复重启。

正常启动应依次看到：

1. `Herdr Panel starting`、`Init PMU SUCCESS!`、`PMIC ready`。
2. `display ready`、`LVGL ready`。
3. `boot built`、`boot frame flushed`、`pages built`、`provision built`、`UI ready`。
4. `startup complete`，随后是 `Herdr Panel up` 或 `Herdr Panel in provisioning mode`。

`boot frame flushed` 表示 LVGL 已尝试把深色启动页发到屏幕。若它出现后屏幕仍全白，优先核对板卡型号、显示排线、SPI/PMIC 错误与供电。若日志停在 `boot built` 后或报 LVGL 内存分配失败，查看 `LVGL heap free` 与 `largest`；若进不了 `PMIC ready`，先查供电和 PMIC I2C。若日志反复从 `Herdr Panel starting` 开始，查首个 panic 或 `ESP_ERROR_CHECK failed`，而非最后一次启动尾部。

硬件不连接到运行构建的机器时，编译成功只能证明代码可构建，不能证明屏幕已显示。需要设备串口日志及白屏持续/重启的现象才能确认最终原因。
