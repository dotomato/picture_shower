# Picture Shower

基于 ESP32-S3 + LVGL 的网络图片相册，自动从服务器下载图片列表并在圆形 AMOLED 屏幕上循环播放。

## 硬件

| 组件 | 规格 |
|------|------|
| 主控 | ESP32-S3（240MHz，16MB Flash，8MB PSRAM） |
| 屏幕 | 2.16 寸圆形 AMOLED（CO5300，480×480，SPI 接口） |
| 触摸 | CST9217（I2C，IRQ 模式） |

## 功能

- **开机初始化界面**：显示启动进度，实时反馈 WiFi 连接状态
- **WiFi 联网下载**：从远程服务器拉取图片列表（`piclist.txt`），并将 JPEG 图片下载到 SPIFFS
- **硬件 JPEG 解码**：使用 ESP32-S3 内置硬件 JPEG 解码器（`esp_new_jpeg`），解码结果存入 PSRAM
- **自动幻灯片播放**：每 3 秒切换一张图片，循环播放
- **离线缓存**：WiFi 连接失败时，自动使用上次下载的本地缓存图片继续播放
- **WiFi 用完即释放**：下载完成后卸载 WiFi 驱动，释放约 40KB 内部 RAM 供显示使用

## 目录结构

```
.
├── main/
│   ├── main.c              # 主程序（WiFi、下载、LVGL 显示）
│   ├── simhei_tomato.c     # 黑体中文字体数据
│   └── sthuop_tomato.c     # 华文仿宋字体数据
├── components/
│   └── esp32_s3_touch_amoled_2_16/   # BSP 板级支持包（显示 + 触摸驱动）
├── assert/
│   └── wifi_config.txt     # WiFi 配置文件（不提交到 Git）
├── partitions.csv          # 分区表（8MB App + 7MB SPIFFS）
├── sdkconfig.defaults      # 关键编译配置
└── sdkconfig               # 完整编译配置
```

## 快速开始

### 1. 配置 WiFi

在 `assert/wifi_config.txt` 中填写 WiFi 信息（该文件不会提交到 Git）：

```
你的WiFi名称
你的WiFi密码
```

### 2. 配置图片服务器

在 `main/main.c` 中修改服务器地址：

```c
#define PICLIST_URL  "http://你的服务器/piclist.txt"
#define PIC_BASE_URL "http://你的服务器/pics/"
```

`piclist.txt` 格式（每行一个文件名）：

```
photo1.jpg
photo2.jpg
photo3.jpg
```

### 3. 编译烧录

```bash
idf.py build flash monitor
```

> 需要 ESP-IDF v5.4.0 及以上版本。

## 内存分配策略

| 区域 | 用途 |
|------|------|
| 内部 RAM | 系统、WiFi 驱动（临时）、DMA 缓冲区 |
| PSRAM | LVGL 帧缓冲、JPEG 解码输出、网络任务栈 |
| SPIFFS | 图片文件缓存（最多 16 张）、WiFi 配置 |

## 分区表

| 分区 | 大小 |
|------|------|
| nvs | 24KB |
| phy_init | 4KB |
| factory (app) | 8MB |
| storage (spiffs) | 7MB |
