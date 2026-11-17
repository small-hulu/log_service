# 更新记录

## 2026-11-17：对齐公共日志协议并完成 SDK 传输验证

- 对齐公共协议 `6562eeb`，增加必填 MessageHeader v2.0，保留 PLOG v1、消息枚举和三类日志 Key。
- 服务统一生成 producer_id、进程 epoch、递增序号和时间戳；ACK 绑定 GET 的客户端身份及逻辑流，校验重放、大小和哈希。
- 调整 END 与等待 ACK 的时序，支持客户端快速确认，提前到达的 ACK 不触发成功。
- 固定 FlatBuffers 25.12.19，提供项目内依赖准备脚本并同步 Python 绑定。
- 增加 Header、生命周期、网络协议及实际 SDK 下载回归；结果与正式联调前提见 [SDK 联调说明](docs/sdk-integration.md)。

## 2026-11-16：切换日志来源并完善编译、运行及 Windows 拉取流程

### 本次修改

- 默认日志源从模拟 hand/glove 目录切换到 `/var/log/poke_recorder`、`/var/log/video_manager`、`/var/log/pico_broker`、`/var/log/dex_hand`，模块名与目录名一致。
- 收集器原有能力可将当前 `.log` 和历史 `.gz` 文件一起打包；仍按目录收集普通文件，没有新增文件名过滤规则。
- 新增根目录 `build.sh`：自动定位工程，执行 CMake 配置、编译和 CTest；默认 Debug、两路并行，可通过环境变量调整。
- 新增根目录 `run.sh`：默认后台启动，支持 start、stop、restart、status、logs，复用已有服务生命周期脚本。重新编译后需手动 restart，未设置开机自启。
- Windows 客户端默认地址调整为 `tcp/10.23.2.11:17447`，默认下载目录调整为 `D:\log_client_test`；历史验收脚本输出放入该目录的 `e2e` 子目录。
- PowerShell 拉取脚本改用工程内 `.venv`，支持 `-PythonExe` 指定已有环境，避免依赖旧电脑的 Python 绝对路径。
- 更新中文 README 和协议说明，明确新模块名、启动步骤、下载命令，以及 ALL/LATEST 的适用范围。

### 验证情况

- RK3588 上编译成功，两组 CTest 全部通过；启动、重启和状态查询通过。
- 在四个目标目录中生成带 `TEST_DATA` 标记的模拟数据，每个目录为一个当前日志及五个 gzip 轮转文件，共 24 个文件，每个文件 1,000 条记录。dex_hand 包含 owner/gateway 两类记录。
- Windows 通过局域网拉取 ALL 成功，交付包 186,831 字节、3 个分块，客户端耗时约 0.250 秒；文件大小、SHA-256、包内清单及 ACK/COMPLETE 确认通过。
- 更新后的 PowerShell 脚本使用指定 Python 环境拉取 dex_hand 成功，默认地址和下载目录生效，收齐该模块六个日志文件并完成校验确认。
- 上述结果是模拟数据的单次功能验证，不代表真实业务日志或大文件性能验收。生成的数据、下载包和本机 Python 环境不进入仓库。

### 当前边界

- 这四个目录目前仍是模拟测试内容；配置使用正式目录不等于已经接入真实业务输出。
- 先使用 ALL。现有 LATEST 依赖 `current_run` 和启动批次目录，新日志布局暂未适配，缺少标记时返回 `LATEST_UNAVAILABLE`。
- 协议继续使用 Zenoh 发布/订阅、FlatBuffers PLOG v1，以及 get、result/{request_id}、ack 三类 Key；未改为单 Key 的原生 Get/Queryable。
- 本次未新增业务日志生成库、轮转管理、断点续传或 SDK 配置下发能力。
