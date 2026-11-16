# 日志交付协议草案 v1

这是独立项目的日志扩展草案，不是已经发布的团队 PCU 协议。文件标识 `PLOG`，版本 1，字段见 `schemas/log.fbs`。SDK 正式接入前应共同评审，再迁入公共协议仓库并对齐公共 MessageHeader。

## SDK 连接信息与接入步骤

以下为当前开发联调配置，不是固定协议常量；部署时由盒子侧提供实际值。

| 信息 | 当前联调值 | 用途 |
|---|---|---|
| 盒子 IP | `10.23.2.11` | 电脑访问盒子的网络地址 |
| Zenoh TCP 端口 | `17447` | 完整连接地址为 `tcp/10.23.2.11:17447` |
| 设备标识 `pcu_id` | `rk3588-log-test` | 组成下方请求、结果和确认 Key；不是密码 |
| 模块名称 `modules` | `poke_recorder`、`video_manager`、`pico_broker`、`dex_hand` | 指定日志来源；空列表表示全部配置来源；需部署新版来源配置 |
| 报文定义 | `schemas/log.fbs`，`PLOG` v1 | SDK 与盒子使用同一版 FlatBuffers 定义 |

连接前提：盒子上的 `log_service` 已启动并监听端口，电脑到盒子网络互通且 TCP 17447 可访问。**ping 通只表示 IP 可达，不代表日志服务或端口已就绪。** SDK 必须通过 Zenoh 建立连接，不能直接向普通 TCP Socket 发送 GET 文本。

当前开发配置未启用身份认证。SDK 不需要 SSH 登录、SSH 账号密码，也不需要知道盒子上的日志路径；路径由盒子侧配置管理。正式部署的认证方式需另行约定。

SDK 获取一次日志的步骤：

1. 连接 `tcp/10.23.2.11:17447`，使用约定的 `pcu_id`。
2. 生成唯一 `request_id`，先订阅对应的 `result/{request_id}` Key，再发布 GET。
3. GET 携带 `request_id`、`mode` 和 `modules`。`ALL` 获取所选模块当前保留的日志；`LATEST` 获取各模块进程本次启动批次的日志，包含该批次轮转文件。当前没有起止时间筛选字段。
4. 接收 BEGIN、CHUNK、END，按块编号合并为 `tar.gz`，核对大小、SHA-256 并验证归档。
5. 发布 ACK，等待 COMPLETE 后认定本次交付成功。错误与超时按下文规则处理。

`tools/sdk_client.py` 是模拟 SDK 的测试客户端，已实现上述流程，可作为正式 SDK 接入参考。默认配置已改为四个真实 `/var/log/` 来源，须同步板端配置并重启服务。真实来源先使用 ALL；缺少 `current_run` 标记和启动批次目录时，LATEST 返回 `LATEST_UNAVAILABLE`。

## Zenoh Key

| Key | 发布方 → 订阅方 | Message.kind |
|---|---|---|
| `poke/pcu/{pcu_id}/logs/get` | SDK → 盒子 | GET |
| `poke/pcu/{pcu_id}/logs/result/{request_id}` | 盒子 → SDK | BEGIN / CHUNK / END / COMPLETE / ERROR |
| `poke/pcu/{pcu_id}/logs/ack` | SDK → 盒子 | ACK |

GET 是业务名称，不是调用 Zenoh 原生 query/get。本版三个入口均为发布订阅，以支持一次请求后的主动文件发送。SDK 必须先订阅自己的 result Key，再发布 GET。request_id 使用 UUID 等全局唯一字串，允许字母、数字、下划线、连字符，最长 96 字节。

## 消息

| kind | 必需业务字段 | 语义 |
|---|---|---|
| GET | request_id, mode, modules | mode=ALL(0)/LATEST(1)；空 modules 表示所有配置来源 |
| BEGIN | request_id, file_name, file_size, chunk_size, chunk_count, sha256 | 压缩完成后的文件元数据，不是过程状态 |
| CHUNK | request_id, chunk_index, data | 原始字节，不用 Base64；编号从 0 开始 |
| END | 与 BEGIN 相同的元数据 | 本轮分块已发送，不代表 SDK 已收到 |
| ACK | request_id, file_size, sha256 | SDK 收齐、校验并验证归档后确认 |
| COMPLETE | 与 BEGIN 相同的元数据 | 服务已接受正确 ACK |
| ERROR | request_id, error_code, detail | 请求失败，无进度信息 |

BEGIN/CHUNK/END 属于文件传输帧；没有“收集中/压缩中/上传中”状态推送。所有字段定义在同一个 FlatBuffers 表中；不同 kind 只使用对应字段。

## 完整性和失败

1. `chunk_count = ceil(file_size / chunk_size)`，除末块之外均为 chunk_size 字节。
2. SDK 按 chunk_index 写入文件偏移，核对块长度；重复块必须内容一致。
3. 收齐所有块并收到 END 后，核对总大小、SHA-256，再发送 ACK。
4. COMPLETE 仅在正确 ACK 后发送。SDK 等待 COMPLETE；若丢失则本版需重新发起新请求，不承诺 exactly-once。
5. 服务等待 ACK 超时后返回 ACK_TIMEOUT 并清理临时包。默认演示配置 5 秒，可改到 1～300 秒。
6. 活动 request_id 的重复 GET 被忽略，原传输继续；最近 128 个结束请求 ID 在当前进程内返回 DUPLICATE，不持久化。新任务在已有任务期间返回 BUSY。
7. 失败后用新 request_id 重试；本版不自动重发、恢复、补块。

主要错误：BUSY、DUPLICATE、UNKNOWN_MODULE、NO_LOGS、LATEST_UNAVAILABLE、LIMIT_EXCEEDED、SOURCE_CHANGED、ARCHIVE、TRANSFER、ACK_TIMEOUT、CANCELLED、INTERNAL。无法解析/缺少有效 ID 的报文丢弃并在服务端记录，不猜测回复路由。

manifest.json 记录 mode、request_id、采集时间、LATEST 的各来源 run_id、相对文件路径、各文件采集字节数及快照语义。包内路径均为来源名加日志相对路径。
