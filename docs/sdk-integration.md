# SDK 联调说明

## 当前结论：初步联调通过（2026-11-17）

- 原版 SDK 已通过公开接口完成设备发现、连接、状态读取及全量/指定模块日志下载。
- 同事 Linux 端使用 7447 下载 ALL，输出 Confirmed archive，完成校验及 ACK/COMPLETE；tar 成功列出四模块日志、dex_hand 仿真运行日志和 manifest.json。
- 同事端本次请求：`234383c475bc4052a01da497d0e31a3b`，压缩包 198,982 字节。依据为同事终端截图。
- 17447 已关闭，扫描和下载统一连接 `tcp/10.23.2.11:7447`。网关提供 discovery/state，log_service 提供日志接口。
- 当前仅为仿真设备和测试日志的链路验收；未完成真实硬件、全部真实业务日志或生产稳定性验收。

## 对齐版本

- 协议：`feat/log-delivery-protocol@6562eeb`，本工程的 `log.fbs`、`common.fbs` 与其一致。
- SDK：`feat/log-sdk@7ee883c`，本轮直接使用该提交的 Python 下载实现，未修改 SDK。
- PLOG `version=1`，公共 Header `major=2/minor=0`。GET、ACK、所有结果消息均包含 Header；旧版无 Header 客户端需升级。
- 服务检查 ACK 的客户端身份、epoch、递增序号、大小和 SHA-256；正确 ACK 后才发送 COMPLETE。

## 当前盒子

| 项目 | 值 |
|---|---|
| 日志端点 | `tcp/10.23.2.11:7447` |
| pcu_id | `rk3588-log-test` |
| modules | `poke_recorder`、`video_manager`、`pico_broker`、`dex_hand`；空列表表示全部 |
| mode | 当前四目录使用 `ALL`；没有 `current_run`，使用 LATEST 会返回 `LATEST_UNAVAILABLE` |
| ACK 等待时间 | END 后 5 秒；大包或较慢电脑可在服务配置中增加 |
| 输出 | Windows `D:\log_client_test`；完整链路证据在 `dex_gateway_20261117` 子目录，统一入口下载在 `router_only_20261117` 子目录 |
| RK 工程 | `/root/xsw/log_service`；`bash run.sh status` 查看状态 |

日志 Key 与此前一致：`poke/pcu/{pcu_id}/logs/get`、`result/{request_id}`、`ack`，均为发布订阅。

## 正式 SDK 的连接前提

当前已部署 dex_hand 网关和仿真手/手套，提供 discovery/state。日志服务以 client 接入同一个 Router，不再监听 17447。正式联调时：

1. 网关和日志服务接入同一个可达的 Zenoh 网络。
2. 日志服务配置的 `pcu_id` 与网关公布的真实 ID 一致。
3. SDK 完成发现和连接后调用 `pcu.logs.download(..., mode=LogMode.ALL)`。

具备上述条件后，在安装了该分支的 SDK 环境运行：

```powershell
python samples/python/logs_download.py rk3588-log-test --connect tcp/10.23.2.11:7447 --mode ALL --output-dir D:\log_client_test
```

已使用未修改的同事 SDK，通过公开接口完成 discover → connection/state → logs.download，以及 SDK 自带 logs_download.py 示例。网关来自 dex_hand 的 fix/gateway-deploy-hardening@6663365；设备为仿真手/手套，不代表真实硬件验收。完整链路证据在 D:\log_client_test\dex_gateway_20261117。

只检查日志服务时，可直接运行本工程客户端：

```powershell
cd D:\code\code\log_service
& .\tools\sdk_get.ps1 -Mode all
```

## 此前独立日志协议验证（2026-11-17，历史记录）

| 测试 | 结果 |
|---|---|
| RK C++ 三组测试 | 全通过；另完成三轮重复测试及 ASan/UBSan 检查 |
| 同事 SDK 实际下载 | 34 次成功：全量、指定模块、约 16 MiB 多分块包及 LATEST 轮转日志 |
| SDK 错误处理 | 6 次预期错误：UNKNOWN_MODULE、NO_LOGS、LATEST_UNAVAILABLE |
| 网络协议回归 | 17 项，部署前后均通过：缺失/无效 Header、BUSY、重复请求、错误身份/epoch/序号/哈希/大小的 ACK、超时及恢复 |
| 重启 | epoch 改变，重启后下载成功 |
| 部署 | 原 17447 服务已更新，临时 17448 服务已停止，无遗留请求工作目录 |

四个 `/var/log/` 目录仍是带 `TEST_DATA` 标记的测试日志；大包和 LATEST 使用 `/userdata` 中的独立测试数据。以上验证的是协议与传输链路，不代表真实业务日志采集和完整网关联调已验收。未执行 SDK 仓库的完整 CI。

可重复执行的测试入口：`tools/test_protocol.py --report <结果.json>`；Python 3.14 环境运行 `tools/test_sdk_download.py --sdk-root <该分支SDK目录> --output <结果目录>`。两者默认连接上述盒子，测试期间应暂停其他下载。

回退备份位于盒子 `/userdata/log_service_protocol_20261117/pre-deploy`，含更新前源码和可执行文件。详细证据见 [protocol-validation.json](protocol-validation.json)。
