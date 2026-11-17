# 构建依赖

- RK3588：Ubuntu 22.04 ARM64、GCC 11、CMake；开发包 `libarchive-dev libssl-dev nlohmann-json3-dev`。
- zenoh-c 1.10.2 standalone 解压到 `deps/zenoh`。[ARM64 包](https://github.com/eclipse-zenoh/zenoh-c/releases/download/1.10.2/zenoh-c-1.10.2-aarch64-unknown-linux-gnu-standalone.zip)，SHA-256：`0fdafdf08cee6f0da361735da159cd6cbe4dfcf351ee578cb0ea9f05ce083b47`。
- FlatBuffers 编译器与 C++ 头文件固定为 **25.12.19**，放在 `deps/flatbuffers`，不使用系统旧版本。
- 本仓库 Windows 测试客户端：Python 3.10+、`eclipse-zenoh==1.10.2 flatbuffers==25.2.10`；同事 SDK 要求 Python 3.14+，实测使用 flatbuffers 25.12.19。

首次在 RK 准备 FlatBuffers：

```bash
python3 tools/fetch_flatbuffers.py
bash build.sh
```

无法直接联网时，下载 [v25.12.19 源码包](https://codeload.github.com/google/flatbuffers/tar.gz/refs/tags/v25.12.19)，再执行：

```bash
TMPDIR=/userdata python3 tools/fetch_flatbuffers.py --archive /userdata/flatbuffers-25.12.19.tar.gz
```

脚本验证 SHA-256：`f81c3162b1046fe8b84b9a0dbdd383e24fdbcf88583b9cb6028f90d04d90696a`。可用 TMPDIR 指定依赖编译的临时目录，用 BUILD_DIR 指定服务的构建目录；run.sh 默认运行 build/log_service。

更新 schema 后同时生成 common 和 log，并同步 Windows 绑定：

```bash
deps/flatbuffers/bin/flatc --python -o tools/generated schemas/common.fbs schemas/log.fbs
```

C++ 绑定由 CMake 自动生成。共享协议来源见 [protocol.md](protocol.md)。
