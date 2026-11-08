# 第三方来源与许可证

[Berry Desk Hub 的 MIT 许可证](LICENSE)适用于项目原创部分。以下第三方代码、几何和字体沿用各自的许可。

| 项目 | Berry Desk Hub 中的用途 | 许可与随附文本 |
| --- | --- | --- |
| [Waveshare Codex Meter](https://github.com/waveshareteam/codex-meter) | ESP32-S3 原始显示固件和 Codex 客户端基础 | MIT · [固件](firmware/UPSTREAM-LICENSE) · [参考客户端](berry-hub-reference-codex-meter/LICENSE) |
| [nasawz/GrokBot](https://github.com/nasawz/GrokBot)、[grokbot-web](https://github.com/Coiggahou2002/grokbot-web) | 小圆球表情几何、眼形和动画算法的来源，适配到原生 C 渲染 | BSD-3-Clause · [版权与许可](firmware/main/GrokBot-LICENSE) |
| [LVGL](https://github.com/lvgl/lvgl) | 屏幕界面、控件和绘制 | MIT · [版权与许可](firmware/licenses/LVGL-MIT.txt) |
| [ESP-IDF](https://github.com/espressif/esp-idf)、[ESP LVGL Adapter](https://components.espressif.com/components/espressif/esp_lvgl_adapter) | ESP32-S3 开发框架和显示任务适配 | Apache-2.0 · [ESP-IDF](firmware/licenses/ESP-IDF-Apache-2.0.txt) · [Adapter](firmware/licenses/ESP-LVGL-Adapter-Apache-2.0.txt) |
| [Waveshare ESP32-S3 AMOLED 板级组件](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_2_16) | 屏幕、触控及板级接口 | Apache-2.0 · [版权与许可](firmware/licenses/Waveshare-BSP-Apache-2.0.txt) |
| [Adobe 思源黑体](https://github.com/adobe-fonts/source-han-sans) | 公开固件的中文点阵字形 | SIL OFL 1.1 · [版权与许可](firmware/licenses/Source-Han-Sans-OFL.txt) |

[ESP-IDF 组件锁定文件](firmware/dependencies.lock)记录了构建使用的组件与版本。固件 ZIP 的 `licenses/` 目录包含构建组件随附的许可文件。
