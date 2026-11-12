# 已验证依赖

2026-11-11 在 RK3588 Ubuntu 22.04 ARM64 原生构建。

- zenoh-c 1.10.2 官方 ARM64 GNU standalone 包：
  https://github.com/eclipse-zenoh/zenoh-c/releases/download/1.10.2/zenoh-c-1.10.2-aarch64-unknown-linux-gnu-standalone.zip
- 已下载归档 SHA-256：`0fdafdf08cee6f0da361735da159cd6cbe4dfcf351ee578cb0ea9f05ce083b47`。
- 解压在 `/root/xsw/log_service/deps/zenoh`，不替换控制工程依赖。
- Ubuntu 开发包：`libarchive-dev libssl-dev flatbuffers-compiler libflatbuffers-dev nlohmann-json3-dev`。
- Windows 独立 venv 内：`eclipse-zenoh==1.10.2 flatbuffers==25.2.10`。
- Python 运行时来自本机已有 Codex runtime，venv 在 `D:\codex_log\tools\zenoh-test`；该基础 Python 被移除后需要重建 venv。

更换 FlatBuffers 编译器时必须同时重新生成 C++ 与 Python schema：

```bash
flatc --cpp --python -o build/generated schemas/log.fbs
```

将生成的 `poke` Python 包同步到电脑的 `tools/generated`。日志协议当前是独立草案，与现有 PCU 消息的 wire version 没有混用。
