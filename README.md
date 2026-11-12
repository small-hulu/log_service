# Poke LogService（第一阶段）

RK3588 上独立运行的 C++17 日志获取服务。SDK 发一个业务 GET 请求，服务收集日志、生成 tar.gz、通过 Zenoh 主动发送数据分块；SDK 校验后 ACK，服务返回 COMPLETE。没有任务进度和状态查询接口。

## 路径与启动

- RK 工程：`/root/xsw/log_service`
- Windows 源码镜像：`D:\code\log_service`
- Windows 测试环境：`D:\codex_log\tools\zenoh-test`
- 接收结果：`D:\codex_log\log-service-received`

RK 上执行：

```bash
cd /root/xsw/log_service
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$PWD/deps/zenoh"
cmake --build build -j2
ctest --test-dir build --output-on-failure
bash tools/service.sh start
bash tools/service.sh status
bash tools/service.sh stop
```

也可前台运行 `build/log_service config/service.json`。启动脚本不安装 systemd，不设置开机启动。业务源目录须事先存在。

Windows PowerShell：

```powershell
& D:\code\log_service\tools\sdk_get.ps1 -Mode all -Modules hand,glove
& D:\code\log_service\tools\sdk_get.ps1 -Mode latest -Modules hand
```

这些命令连接 `tcp/10.22.69.16:17447`，PCU ID 为 `rk3588-log-test`。结果为压缩包路径、SHA-256、分块数、包内清单。工具读取压缩包以验证内容，不自动解压到磁盘。

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

参考基线：`dex_hand` 的 `49dc1d5`（runtime/codec/业务层分离）、`poke-communication-protocol` 的 `8e15aaf`（FlatBuffers、PCU Key 命名）。没有修改两个参考仓库。

## ALL / LATEST

- ALL：选定来源目录内仍保留的普通文件；包括历史启动与当前启动日志。
- LATEST：每个来源的 `current_run` 文件中指定的启动批次目录，包括该批次内轮转文件。
- 空模块列表表示全部配置来源。请求不能指定任意服务器路径。
- 一个来源对应一个日志生产进程；多个进程应分别登记来源。

```text
hand/
  current_run          内容：run_20261111_001
  run_20261110_001/    历史启动日志
  run_20261111_001/    当前启动日志及其轮转文件
```

生产进程启动时创建新批次目录，并原子替换 `current_run` 标记。必须由实际生产进程/启动管理者维护；日志服务不会猜测进程启动时间。缺少标记时 LATEST 返回 `LATEST_UNAVAILABLE`。部署前需将现有模块接入这一约定，或增加对应的收集适配实现。

快照是逐文件记录大小并复制该长度的内容，采集后新增内容不纳入此包；不是跨进程原子快照，最后一行可能不完整。支持追加写和 rename 轮转；不保证 copytruncate、原地改写文件时的一致性。检测到截断或消失会失败，调用方用新请求 ID 重试。跳过软链接，拒绝硬链接；请求清理只操作自己的临时目录，不删除源日志。

## 依赖与范围

- RK：GCC 11.4、CMake、zenoh-c 1.10.2（项目内 `deps/zenoh`）、FlatBuffers 1.12.1、libarchive 3.6、OpenSSL 3、nlohmann-json。
- PC 工具：Python、eclipse-zenoh 1.10.2、flatbuffers 25.2.10；Python 只用于 SDK 模拟器，不用于 RK 服务实现。
- `.fbs` 构建时生成 C++ / Python 代码，电脑工具使用 `tools/generated` 中同一份 schema 的生成物。
- 安装依赖时 Ubuntu 的 libarchive/libssl 运行库随开发包更新了补丁版本。

当前默认输入是 `runtime/fixtures` 的合成日志，不是现场手/手套日志。合成数据由 `tools/create_fixtures.py` 生成。业务轮转、logrotate 配置、统一 spdlog 写入、正式 SDK 接入及共享协议仓库发布均未实现。

LogRotationManager 是下一阶段的模块边界，本版没有空壳实现，也不接管现有模块轮转。同一文件只能由一个轮转机制管理。

## 运行限制

- 同时一个任务；其他请求返回 BUSY。最多 256 MiB 源文件、10000 个文件、64 KiB/块、4 MiB/s 发送速率（可配置）。临时磁盘还需要快照与压缩包的空间。
- ACK 超时、成功、普通异常及 SIGTERM 会清理请求临时目录；SIGKILL/掉电遗留目录需人工确认后清理，本版不自动恢复。
- 网络使用独立 TCP 17447，不改变原控制程序端口，不访问 CAN。当前为可信开发网直连配置，尚未接入团队身份认证/TLS/访问控制。
- 第一版没有断点续传和分块补发；接收端发现缺块、超时或校验失败须用新 request_id 重试完整请求。不能把发布成功视为已交付。

协议详见 [docs/protocol.md](docs/protocol.md)，实测记录见 [docs/e2e-results.json](docs/e2e-results.json)。

## 重跑网络验收

RK 上先停止本服务，再用包含空来源和缺少启动标记来源的验收配置启动：

```bash
bash tools/service.sh stop
bash tools/service.sh start /root/xsw/log_service/config/e2e.json
```

电脑运行：

```powershell
& D:\codex_log\tools\zenoh-test\Scripts\python.exe D:\code\log_service\tools\run_e2e.py
```

完成后在 RK 执行 `bash tools/service.sh stop`、`bash tools/service.sh start`，恢复只有 hand/glove 的默认演示配置。首次安装需要在项目目录执行 `python3 tools/create_fixtures.py`。测试数据不可当作真实业务日志验收。
