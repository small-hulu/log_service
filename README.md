# Poke LogService（第一阶段）

RK3588 上独立运行的 C++17 日志获取服务。SDK 发一个业务 GET 请求，服务收集日志、生成 tar.gz、通过 Zenoh 主动发送数据分块；SDK 校验后 ACK，服务返回 COMPLETE。没有任务进度和状态查询接口。

## 路径与启动

- RK 工程：`/root/xsw/log_service`
- Windows 源码镜像：`D:\code\code\log_service`
- Windows 推荐测试环境：工程内 `.venv`；已有环境可通过 `-PythonExe` 指定
- 接收结果：`D:\log_client_test`

RK 上执行：

```bash
cd /root/xsw/log_service
python3 tools/fetch_flatbuffers.py  # 首次准备协议要求的 FlatBuffers 25.12.19
bash build.sh             # 配置、编译，并自动执行三组 CTest
bash run.sh               # 默认后台启动；已运行时不重复启动
bash run.sh status        # 查看状态
bash run.sh logs          # 查看最近 50 行并持续跟踪，Ctrl+C 仅退出查看
bash run.sh restart       # 重新编译后重启，使新程序生效
bash run.sh stop          # 停止服务
```

编译默认使用 Debug 和两个并行任务，可用 `BUILD_TYPE=Release JOBS=2 bash build.sh` 调整。编译不会自动重启服务。

也可先停止后台服务，再前台运行 `build/log_service config/service.json`。启动脚本不安装 systemd，不设置开机启动；关闭 SSH 不影响后台服务。业务源目录须事先存在。

Windows PowerShell：

```powershell
cd D:\code\code\log_service
# 首次使用：创建环境并安装依赖（python 须为已安装的 Python 解释器）
python -m venv .venv
& .\.venv\Scripts\python.exe -m pip install eclipse-zenoh==1.10.2 flatbuffers==25.2.10

# 获取四个模块的全部保留日志
& .\tools\sdk_get.ps1 -Mode all
# 只获取指定模块
& .\tools\sdk_get.ps1 -Mode all -Modules poke_recorder,dex_hand
# 也可直接使用 Python 客户端
& .\.venv\Scripts\python.exe .\tools\sdk_client.py --endpoint tcp/10.23.2.11:17447 --mode all --output D:\log_client_test
```

默认连接 `tcp/10.23.2.11:17447`，PCU ID 为 `rk3588-log-test`。IP 是当前开发盒子的配置，可通过 `-Endpoint` / `--endpoint` 修改；输出目录可通过 `-Output` / `--output` 修改。结果为压缩包路径、SHA-256、分块数、包内清单。工具读取压缩包以验证内容，不自动解压到磁盘。

## 模块边界

```text
PC SDK / 测试客户端
       ↕ Zenoh + FlatBuffers
LogGateway（Runtime / Codec）
       ↕ 普通 C++ Message
LogService（单后台工作线程，调度与确认）
       ├── LogCollector → 文件快照、manifest.json
       ├── LogArchiver → libarchive tar.gz
       └── LogTransfer → 限速分块发送 → LogTransport
```

`include/poke/log` 不包含 Zenoh 或 FlatBuffers 类型。具体运行时隐藏在 Pimpl 内；业务、收集、压缩和传输可以独立测试。采用 RAII 管理文件、归档器、实例锁与请求工作目录。没有复制控制会话、CAN、手或手套的业务代码。

协议基线：`poke-communication-protocol` 的 `feat/log-delivery-protocol@6562eeb`；对接 SDK：`poke-hand-sdk` 的 `feat/log-sdk@7ee883c`。所有消息使用 PLOG v1 + 公共 MessageHeader v2.0，拒绝没有 Header 的旧报文。

正式 SDK 的 `PokeClient` 需要先完成 discovery/state。日志服务只提供日志 Key；网关与日志服务须使用一致的 `pcu_id`，并能通过同一个 Zenoh 网络访问。仅连接本服务的 17447 端口不能代替设备发现。当前四目录先使用 `LogMode.ALL`，SDK 默认的 LATEST 需要来源维护 `current_run`。

## ALL / LATEST

- ALL：选定来源目录内仍保留的普通文件；包括历史启动与当前启动日志。
- LATEST：每个来源的 `current_run` 文件中指定的启动批次目录，包括该批次内轮转文件。
- 空模块列表表示全部配置来源。请求不能指定任意服务器路径。
- ALL 按来源目录收集，`dex_hand` 可包含 owner/gateway 的日志。LATEST 的每个来源只能表达一个启动批次；多个独立进程若没有共同批次约定，须拆分来源或增加适配。

```text
hand/
  current_run          内容：run_20261111_001
  run_20261110_001/    历史启动日志
  run_20261111_001/    当前启动日志及其轮转文件
```

生产进程启动时创建新批次目录，并原子替换 `current_run` 标记。必须由实际生产进程/启动管理者维护；日志服务不会猜测进程启动时间。缺少标记时 LATEST 返回 `LATEST_UNAVAILABLE`。部署前需将现有模块接入这一约定，或增加对应的收集适配实现。

快照是逐文件记录大小并复制该长度的内容，采集后新增内容不纳入此包；不是跨进程原子快照，最后一行可能不完整。支持追加写和 rename 轮转；不保证 copytruncate、原地改写文件时的一致性。检测到截断或消失会失败，调用方用新请求 ID 重试。跳过软链接，拒绝硬链接；请求清理只操作自己的临时目录，不删除源日志。

## 依赖与范围

- RK：GCC 11.4、CMake、zenoh-c 1.10.2（项目内 `deps/zenoh`）、FlatBuffers 25.12.19（`deps/flatbuffers`）、libarchive 3.6、OpenSSL 3、nlohmann-json。
- PC 工具：Python、eclipse-zenoh 1.10.2、flatbuffers 25.2.10；Python 只用于 SDK 模拟器，不用于 RK 服务实现。
- `.fbs` 构建时生成 C++ / Python 代码，电脑工具使用 `tools/generated` 中同一份 schema 的生成物。
- 安装依赖时 Ubuntu 的 libarchive/libssl 运行库随开发包更新了补丁版本。

默认 `config/service.json` 已改为真实日志目录：

| 模块名（SDK modules） | 来源 | 目录与预期文件 |
|---|---|---|
| poke_recorder | poke_recorder.service | `/var/log/poke_recorder/poke_recorder.log` 及 `.1.gz` 至 `.5.gz` |
| video_manager | video-manager.service | `/var/log/video_manager/video_manager.log` 及 `.1.gz` 至 `.5.gz` |
| pico_broker | pico-broker.service | `/var/log/pico_broker/pico_broker.log` 及 `.1.gz` 至 `.5.gz` |
| dex_hand | owner / gateway | `/var/log/dex_hand/dex_hand.log` 及 `dex_hand.log.<N>.gz` |

收集器按目录递归收集普通文件，没有按上述文件名过滤；`.gz` 文件原样放入交付包。目录须存在，且仍受 256 MiB 源文件总量限制。真实日志先使用 `--mode all`；没有 `current_run` 标记及启动批次目录的来源不支持当前 LATEST 语义。不要用“当前活动文件”冒充“本次启动日志”。此配置变更须同步到板端并重启服务才生效。

合成测试数据可由 `tools/create_fixtures.py` 生成，用于独立测试配置。该服务不负责业务日志写入和轮转；SDK 对接结果与连接前提见 [联调说明](docs/sdk-integration.md)。

LogRotationManager 是下一阶段的模块边界，本版没有空壳实现，也不接管现有模块轮转。同一文件只能由一个轮转机制管理。

## 运行限制

- 同时一个任务；其他请求返回 BUSY。最多 256 MiB 源文件、10000 个文件、64 KiB/块、4 MiB/s 发送速率（可配置）。临时磁盘还需要快照与压缩包的空间。
- ACK 超时、成功、普通异常及 SIGTERM 会清理请求临时目录；SIGKILL/掉电遗留目录需人工确认后清理，本版不自动恢复。
- 网络使用独立 TCP 17447，不改变原控制程序端口，不访问 CAN。当前为可信开发网直连配置，尚未接入团队身份认证/TLS/访问控制。
- 第一版没有断点续传和分块补发；接收端发现缺块、超时或校验失败须用新 request_id 重试完整请求。不能把发布成功视为已交付。

协议详见 [docs/protocol.md](docs/protocol.md)，历史模拟数据验收记录见 [docs/e2e-results.json](docs/e2e-results.json)，本次更新及四目录验证结果见 [更新记录](CHANGELOG.md)。

## 重跑网络验收

RK 上先停止本服务，再用包含空来源和缺少启动标记来源的验收配置启动：

```bash
bash tools/service.sh stop
bash tools/service.sh start /root/xsw/log_service/config/e2e.json
```

电脑运行：

```powershell
cd D:\code\code\log_service
& .\.venv\Scripts\python.exe .\tools\run_e2e.py
```

完成后在 RK 执行 `bash run.sh restart`，恢复四个 `/var/log/` 来源的默认配置。首次运行模拟数据验收前，需要在项目目录执行 `python3 tools/create_fixtures.py`。该脚本只创建旧版 fixtures，不生成四个业务目录下的文件。测试数据不可当作真实业务日志验收。
