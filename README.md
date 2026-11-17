# Poke LogService

RK3588 上的 C++17 日志获取服务。按模块收集日志并打包为 tar.gz，通过 Zenoh 分块传输；SDK 完成大小、SHA-256 校验及 ACK/COMPLETE 确认后，下载完成。

当前部署已于 **2026-11-17 完成 SDK 端到端联调**：设备发现、状态读取、全量/指定模块下载通过，同事 Linux 端已确认下载包并查看四模块文件清单。本说明固化当前可用的部署和接入方式；验证使用仿真设备和测试日志，不等同于真实硬件或生产稳定性验收。

## 通信与配置

```text
SDK → Zenoh Router :7447
             ├─ dex_hand 网关：设备发现、状态
             └─ log_service：日志收集、打包、传输
```

| 配置 | 当前值 |
|---|---|
| SDK 连接地址 | `tcp/10.23.2.11:7447` |
| PCU ID | `rk3588-log-test`，必须与 dex_hand 网关一致 |
| 板端连接 | client → `tcp/127.0.0.1:7447`，不监听 17447 |
| 日志模式 | 当前四目录使用 `ALL` |
| 打包临时目录 | `/userdata/log_service/work` |
| Windows 下载目录 | `D:\log_client_test` |

`config/service.json` 管理 PCU ID、日志来源和传输限制；`config/zenoh.json5` 管理网络连接。修改后重启日志服务。迁移到其他设备时同步调整路径、PCU ID 和 Router 地址。

## 编译与部署

板端依赖：GCC、CMake、zenoh-c 1.10.2、FlatBuffers 25.12.19、libarchive、OpenSSL、nlohmann-json。当前盒子的依赖已准备，zenoh-c 位于 `deps/zenoh`。

```bash
cd /root/xsw/log_service
python3 tools/fetch_flatbuffers.py   # 首次准备 FlatBuffers
bash build.sh                      # 编译并执行 CTest
```

默认 Debug、两个编译任务；可用 `BUILD_TYPE=Release JOBS=2 bash build.sh` 调整。编译不会自动重启运行中的服务。

当前盒子已有 dex_hand 网关、Router 和 `poke-log-test.target`。首次将日志服务接入启动组，使用 root 执行：

```bash
bash run.sh stop
cp config/log_service.service /etc/systemd/system/
mkdir -p /etc/systemd/system/poke-log-test.target.d
cp config/poke-log-test.target.d/log-service.conf /etc/systemd/system/poke-log-test.target.d/
systemctl daemon-reload
systemctl start poke-log-test.target
```

这些单元文件使用当前盒子的工程路径和服务名，迁移部署时需要调整。未设置开机自启。

## 启动与管理

```bash
systemctl start poke-log-test.target   # 启动 Router、网关、仿真设备和日志服务
systemctl stop poke-log-test.target    # 停止整组服务

cd /root/xsw/log_service
bash run.sh status                     # 查看日志服务
bash run.sh restart                    # 重启日志服务，使配置或新程序生效
bash run.sh logs                       # 跟踪日志；Ctrl+C 退出查看
```

安装对应 systemd 单元后，`run.sh` 自动交由 systemd 管理；未安装时使用后台进程。服务运行日志位于 `runtime/service.log`。

## SDK 下载

SDK 先执行 discovery，再连接 PCU、读取状态，最后调用 `pcu.logs.download(..., mode=LogMode.ALL)`。使用 SDK 自带示例：

```bash
python samples/python/logs_download.py rk3588-log-test --connect tcp/10.23.2.11:7447 --mode ALL --output-dir ./downloads
```

Windows 也可使用本仓库的独立日志客户端：

```powershell
cd D:\code\code\log_service
python -m venv .venv
& .\.venv\Scripts\python.exe -m pip install eclipse-zenoh==1.10.2 flatbuffers==25.2.10
& .\tools\sdk_get.ps1 -Mode all
& .\tools\sdk_get.ps1 -Mode all -Modules poke_recorder,dex_hand
```

默认保存到 `D:\log_client_test`；通过 `-Endpoint`、`-Output` 修改地址和输出目录。该客户端只验证日志接口，不代替正式 SDK 的设备发现。压缩包包含日志文件和 `manifest.json`，不会自动解压。

## 日志来源

| modules 名称 | 收集目录 |
|---|---|
| `poke_recorder` | `/var/log/poke_recorder` |
| `video_manager` | `/var/log/video_manager` |
| `pico_broker` | `/var/log/pico_broker` |
| `dex_hand` | `/var/log/dex_hand`，包含 owner/gateway 仿真运行日志 |

modules 留空表示全部来源。新增来源时在 `config/service.json` 的 `sources` 中增加 `name` 和 `root`，确保目录存在且可读，然后重启服务。收集器递归收集普通文件，历史 `.gz` 原样打包，跳过软链接并拒绝硬链接。日志写入和轮转由业务日志组件负责。

## 能力与限制

- `ALL`：收集所选目录内保留的日志。`LATEST`：收集来源的 `current_run` 指定的启动批次；当前四目录未维护该标记，使用 LATEST 返回 `LATEST_UNAVAILABLE`。
- 同时处理一个任务，其他请求返回 BUSY。默认最多 256 MiB 源文件、10000 个文件、64 KiB/块、4 MiB/s，ACK 等待 5 秒，可通过配置调整。
- 逐文件快照支持追加写和 rename 轮转，不是跨进程原子快照；不保证 copytruncate 或原地改写的一致性。打包需要额外临时磁盘空间。
- 不支持断点续传、分块补发或任务进度查询。失败时使用新 request_id 重试；SIGKILL/掉电遗留临时目录需人工检查。
- 当前部署使用可信开发网络，未配置身份认证、TLS 或访问控制。

协议基线：`feat/log-delivery-protocol@6562eeb`；实测 SDK：`feat/log-sdk@7ee883c`。日志消息使用 PLOG v1 和公共 MessageHeader v2.0。

详细定义见 [协议说明](docs/protocol.md)，测试结果见 [SDK 联调说明](docs/sdk-integration.md)，版本变更见 [更新记录](CHANGELOG.md)。
