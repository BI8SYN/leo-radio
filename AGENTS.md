# AI 协作说明

## Ubuntu 构建（默认入口）

- 默认在 OrbStack Ubuntu `embedded` 中构建，Linux 用户 `johnson`；源码保留在 Mac 可直接访问的目录。
- 从仓库根目录运行 `tools/build-ubuntu.sh`，不要默认调用 macOS 工具链、Docker 或旧的本机构建入口。
- 默认配置：`default`。可选配置：`default`；以 `tools/build-ubuntu.sh --help` 为准。
- Mac 调用会自动进入 Ubuntu；在 Ubuntu 内执行同一脚本也可以。机器或用户名变化时，可设置 `EMBEDDED_MACHINE` / `EMBEDDED_USER`；并行数用 `BUILD_JOBS`，默认 4。
- 工具链与依赖在 Linux 内，构建缓存位于 `~/build/leo-radio/<checkout-hash>/<variant>/`。不同工作副本使用独立缓存。
- 成功产物导出到本项目 `.ubuntu-build/<variant>/`，查看 `build-info.json` 和 `SHA256SUMS`；命令失败时不得把旧固件当成新结果。
- 此入口只执行构建／本机测试，不执行烧录、串口监视、签名、发布、Git 提交或推送。硬件验证按用户当前任务和本文件已有授权另行执行。
- 不复制 macOS 的可执行工具或 Python venv 到 Linux；工具链升级需遵守项目现有版本要求。
- ESP-IDF 固定使用 v5.5.4（默认 `~/esp/esp-idf`），Python 环境为 `~/.espressif/python_env/idf5.5_py3.12_env`；脚本复制 sdkconfig 到 Linux 构建目录，避免改写仓库里的板卡配置。
- Linux 构建也使用独立的 `dependencies.lock` 副本，避免因 SDK 版本或换行符差异回写仓库。若任务主动修改组件依赖，应审核 Linux 构建目录里的锁文件，再有意同步回项目。

## embedded 设备操作（默认）

- 编译、烧录、Flash 备份与串口监视统一在 OrbStack `embedded` / `johnson` 内运行。Mac 只运行系统 Python + OrbStack 分发，不使用 Mac ESP-IDF、PlatformIO 或其 Python 环境，也不得默默回退到本机工具。
- 统一入口 `tools/device-ubuntu.sh --help`。`ports` 列出 Linux 串口，`version` 检查 Linux 工具。`ESP_PORT` 指定 Linux 设备路径，只有恰好一个设备时才自动选择；多个设备必须明确指定。
- USB 串口可能自动转发为 `/dev/cu.usbmodem…`，USB 独占直通后一般是 `/dev/ttyACM0` / `/dev/ttyUSB0`。用 `orb serial list` / `orb usb list` 核对，禁止把构建或 `--help` 当作设备验证。 需要独占直通时，先核对目标设备的 VID/PID/序列号，再执行 `orb usb attach <USB-ID> -m embedded`，随后重新运行 `ports`；不得选择外挂存储设备。
- `EMBEDDED_VARIANT` 选择与 `build-ubuntu.sh` 相同的板型；radio 也支持 `RADIO_BOARD`。跨板型不可复用烧录地址。默认 esptool 波特率 115200；实测稳定的原生 USB 可显式 `ESP_BAUD=921600`，串口监视默认 115200，Ctrl-] 退出。
- Mac 的绝对文件路径会映射到 Linux `/mnt/mac/…`；从项目根目录调用入口，源码和输出仍然由 Mac/Codex 直接访问。

```bash
tools/device-ubuntu.sh info
tools/device-ubuntu.sh backup backup/标签/flash.bin 0x1000000  # 容量必须按实际芯片核对
tools/device-ubuntu.sh flash .ubuntu-build/配置/应用.bin 0x实际应用地址
tools/device-ubuntu.sh verify .ubuntu-build/配置/应用.bin 0x实际应用地址
tools/device-ubuntu.sh monitor
```

- 上述容量、镜像名和地址是占位示例；操作前核对芯片、当前分区表与活动 OTA 槽，先备份。默认只更新明确指定的应用范围，保留 NVS/Wi-Fi/收藏；不能因为示例地址就写整个 Flash。
- `backup` 使用单块在途读取、逐块摘要、重试和整片设备摘要验证；中断后可使用同一路径继续，但会核对设备身份。备份文件含私有配置，必须保持私有权限，禁止提交。
- `tools/esp.sh` 保留 esptool 子命令兼容入口，实际执行仍在 Linux。`tools/device-ubuntu.sh python tools/脚本.py …` 可运行依赖 pyserial 的辅助工具，设备端口仍需显式核对。

- `tools/idf.sh build` 委托 `build-ubuntu.sh`，`where` 输出该配置的 Linux 构建目录；`merge-bin` 等打包命令使用同一 Linux 缓存，避免误读旧的 Mac build/。
- 旧 `idf.sh flash` / `app-flash` 可能覆盖 boot/OTA 布局，默认拒绝。确实需要整套 IDF 布局时，先审核 flasher_args.json 并显式设置 `EMBEDDED_FULL_FLASH=1`；日常升级优先用明确镜像和地址的 `device-ubuntu.sh flash`。
- 打包脚本从 Mac 调用时自动到 Linux，合并及哈希计算也在 Linux。签名、发布、部署和硬件授权仍遵守本文件原有门禁，不因环境迁移而扩大权限。

- GitHub Actions 的 IDF Linux 容器继续使用已激活的 IDF_PATH/Python 和仓库 build/；这是已有 CI 发布流程，Mac 日常开发仍默认 embedded。
