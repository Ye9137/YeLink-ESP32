# YeLink ESP32 图文上手教程

从接线到手机联网。首次建议使用路由模式，它不需要云服务器或设备令牌。

## 1. 准备开发环境

安装 [ESP-IDF v6.1](https://github.com/espressif/esp-idf/tree/v6.1)，并按 [官方入门文档](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/get-started/) 安装经典 ESP32 的工具链。macOS/Linux 在已安装的 IDF 目录执行 `. ./export.sh`；Windows 使用 ESP-IDF 提供的命令行环境。

运行 `idf.py --version` 确认环境。该项目使用 ESP-IDF 6.x API；不要直接使用 Arduino IDE 编译。组件管理器会按 `dependencies.lock` 下载 cJSON，首次构建需要网络。

## 2. 断电接线

![逻辑接线图](images/wiring.svg)

| ST7735 屏幕 | ESP32 | 用途 |
| --- | --- | --- |
| GND | GND | 公共地 |
| VDD / VCC | 3V3 | 3.3V 供电 |
| SCL / SCK | GPIO14 | SPI 时钟 |
| SDA / MOSI | GPIO13 | SPI 数据 |
| RST / RES | GPIO25 | 复位 |
| DC / A0 | GPIO26 | 数据 / 命令 |
| CS | GPIO27 | 片选 |
| BLK / LED | 经 100Ω～220Ω 电阻接 3V3 | 背光，按实际模块规格调整 |

按开发板丝印查找 GPIO，不能按图中方框的位置数针脚。SCL/SDA 在这里不是 I²C。不要把裸 3.3V 屏幕接到 VIN/5V。接好后再通过 USB 给开发板供电。

## 3. 下载源码并准备动画

```sh
git clone https://github.com/Ye9137/YeLink-ESP32.git
cd YeLink-ESP32
python3 scripts/prepare_demo_animation.py
```

脚本生成一个原创的几何信号条动画，输出 `main/moon_cat_gif.bin` 和配套的 `main/moon_cat_gif_data.h`。这两个文件必须匹配。为保护本地素材，脚本检测到任一文件存在时会停止；需要重新生成时先备份并移走这两个文件。

使用自己的 GIF（需要 Pillow）可改为：

```sh
python3 -m pip install Pillow
python3 scripts/prepare_cat_gif.py /path/to/your-licensed-animation.gif
```

该转换命令会覆盖上述两个生成文件，并在 `build/moon-cat-original-preview/` 输出预览。GIF 的色彩会量化为 RGB565；文件名保留历史兼容命名，不要求素材必须是猫。请只使用自己拥有权利或得到许可的素材。

## 4. 编译

```sh
idf.py set-target esp32
idf.py -B build/default menuconfig
idf.py -B build/default build
```

`menuconfig` 可选。`Ye display` 可设置 SPI 时钟（默认请求 27MHz）和动画目标 FPS；`Ye Wi-Fi sharing` 可设置 NAPT 容量（默认 512）、UDP 超时、TCP MSS 和 HT40。初次使用默认值即可。

本项目的默认 CPU 频率为 240MHz，保留看门狗与欠压保护。27MHz 是 SPI 请求时钟，不是屏幕刷新率。若花屏，可在 `Ye display` 降到 20MHz 或 10MHz。

## 5. 烧录和查看日志

先确认 USB 串口：macOS 可运行 `ls /dev/cu.*`，Linux 可检查 `/dev/ttyUSB*` 或 `/dev/ttyACM*`，Windows 在设备管理器查看 COM 端口。

```sh
idf.py -B build/default -p YOUR_SERIAL_PORT flash monitor
```

若无法连接，可降低波特率：

```sh
idf.py -B build/default -p YOUR_SERIAL_PORT -b 115200 flash monitor
```

按 `Ctrl+]` 退出监视器。自动下载失败时，按住板上的 BOOT 按钮再尝试连接，连接成功后松开。完整 `flash` 会更新应用及分区表；不需要执行 `erase-flash`，后者会删除已保存的配置。

## 6. 手机配网与共享

1. 连接热点 **Ye的共享wifi**，默认 WPA2 密码 **11111111**。
2. 系统弹出配网页，或手动访问 **http://192.168.4.1**。手机提示“无互联网”时，先保持连接以完成配置。
3. 选择 **单纯 Wi-Fi 路由器**，填写可正常上网的 2.4 GHz Wi-Fi 名称与密码。
4. 勾选网络共享并保存。设备会重启，手机需要重新连接热点。
5. 再次打开配网页，确认上游取得 IP、共享已开启及 DNS 已下发。
6. 在手机上打开网页或下载小文件，核实实际转发。需要严格验证时暂时关闭手机移动数据，避免系统绕过 Wi-Fi。

路由模式取得上游 IP 后，小屏切换为速率、CPU、内存及网络日志；丢失 IP 时回到动画。该判断不检测公网可达性，上游 Wi-Fi 有 IP 但互联网中断时，仍可能显示状态页。

默认密码是公开的演示配置。部署前修改 `main/pet_main.c` 中的 `ap_name` 和 `ap_password`，使用 8～63 字节 WPA2 密码后重新编译烧录。配置页面通过本地 HTTP 访问，热点接入者可操作配置；仅向可信设备提供热点。

## 7. 云端模式（可选）

云端模式需要自己部署兼容的 HTTPS 服务端，本仓库只含设备固件。教程使用占位地址 `https://example.com/esp32`，并不提供可直接使用的公共后台或令牌。

在配网页选择 **云端控制屏幕**，填写上游 Wi-Fi、实际后台地址及设备令牌。服务器需具备有效证书，实现固件使用的指令查询、资源下载和状态上报协议。细节可从 `main/pet_main.c` 的 `poll_task`、`apply_command` 和 `request` 查看。

云端模式关闭热点互联网转发。不要把设备令牌写入提交、截图或公开日志；通过设备本地配网页保存到 NVS。

## 8. 常见问题

| 现象 | 优先检查 |
| --- | --- |
| 编译提示缺少动画 | 先运行示例动画生成脚本，确认 `.bin` 和配套头文件同时存在 |
| 找不到 `idf.py` | 激活 ESP-IDF 环境；Windows 用 ESP-IDF 终端 |
| 屏幕不亮 | 3.3V、共地、背光电阻与 BLK 接线 |
| 白屏或花屏 | 屏幕型号、DC/CS/RST 接线、SPI 频率及杜邦线长度 |
| ESP32 重启 / brownout | USB 供电、线材、电压及串口日志；不要关闭欠压保护掩盖供电问题 |
| 手机连上但不能上网 | 上游本身可上网、共享开关、DHCP/DNS、是否与 `192.168.4.0/24` 冲突 |
| 配网页打不开 | 手机仍连接 ESP32 热点，访问 HTTP 而非 HTTPS；检查 VPN 是否接管局域网 |
| 代理/VPN 连接异常 | 暂时关闭代理作对照，再参考根目录的 Clash 兼容片段，先备份原配置 |
| 网速慢 | 对比相同客户端和上游，观察 RSSI、NAPT 淘汰、堆空间；单无线电转发存在额外开销 |

`Clash-ESP32-兼容片段.yaml` 是可选参考，需要按自己的客户端版本合并，不应覆盖整个订阅配置。

## 9. 验证与反馈

主机回归测试：

```sh
python3 scripts/test_router_tcp_mss.py
python3 scripts/test_router_ip4.py
```

需要支持 ASan/UBSan 的 C 编译器。实际构建、屏幕和网络验证的边界见 [验证记录](validation.md)。反馈时提供固件版本、ESP-IDF 版本、硬件型号、复现步骤及脱敏日志。
