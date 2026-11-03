# Native URMA experimental server transport

本目录支持固定 SDK 编译检查和内网验收；不表示已通过 UB 硬件认证。
在 Valkey `ConnectionType` 层接入 liburma，使用双边 SEND/RECV。
普通 TCP/CLI 不会自动升级为 URMA，未实现原生 libvalkey/CLI adapter。
`client-probe` 是专项验收客户端，不替代这两个后续工作。

## 取得代码与构建

```sh
git clone --branch work/urma-native-design https://github.com/fengjiuko/valkey-ub.git
cd valkey-ub
make -j4 MALLOC=libc BUILD_URMA=yes
make -C tests/urma
build/urma-runtime/sdk-probe --link-only
build/urma-runtime/sdk-probe --inventory
```

默认构建不依赖 SDK；启用时需要 Linux 和实际 liburma 开发包。
默认头文件目录为 `/usr/include/ub/umdk/urma`，链接 `-lurma`。
开发参考 SHA：`7c8a9541cba083c22ffc55596f7937f991eeae1a`；实际包、provider、
驱动、固件与硬件组合仍须在内网锁定。本地只编译了该快照的 core/common
原始 CMake target，未构建或验证 UDMA/provider，也没有把 SDK vendoring 进仓库。

自定义安装路径时，向两个 Make 入口都传递**绝对路径**，例如：

```sh
make -j4 MALLOC=libc BUILD_URMA=yes \
  URMA_CFLAGS='-isystem /opt/umdk/include/ub/umdk/urma' \
  URMA_LIBS='-L/opt/umdk/lib64 -Wl,-rpath,/opt/umdk/lib64 -lurma'
make -C tests/urma \
  URMA_CFLAGS='-isystem /opt/umdk/include/ub/umdk/urma' \
  URMA_LIBS='-L/opt/umdk/lib64 -Wl,-rpath,/opt/umdk/lib64 -lurma'
cmake -S . -B build/native -DBUILD_MALLOC=libc -DBUILD_URMA=ON \
  -DURMA_INCLUDE_DIR=/opt/umdk/include/ub/umdk/urma \
  -DURMA_LIBRARY=/opt/umdk/lib64/liburma.so
cmake --build build/native --parallel 4
```

依赖库也必须可由动态加载器找到；记录 `ldd` 和实际 SONAME。
切换 Make 构建选项时按仓库惯例使用干净构建。
`--inventory` 无设备或 SDK 初始化失败时退出非零；这不是测试通过。

## 内网双机验收

先记录两端 `uname -a`、`getconf PAGESIZE`、SDK 包版本、provider/驱动/固件、
设备名及 EID 索引，运行官方 SDK 样例，再执行本探针。
首版仅接受 `URMA_TRANSPORT_UB`，并检查 RC、RTP、目标顺序、队列/消息上限。
不满足条件就拒绝；不会选择 SoftUB、IB、UMS 或 TCP 业务路径。
这些检查只确认设备报告的能力，不能替代顺序、故障和销毁语义的双机验证。

服务端测试配置（所有 `urma-*` 配置均需重启）：

```conf
bind 192.0.2.10
port 6379
urma-port 6380
urma-device <盘点得到的原生UB设备名>
urma-eid-index <盘点得到的索引>
urma-buffer-limit 64mb
urma-progress-timeout 10000
urma-allow-insecure-bootstrap yes
io-threads 1
save ""
appendonly no
protected-mode yes
aclfile /etc/valkey/users.acl
```

将示例地址、设备及路径替换为实际值，并配置测试 ACL 用户。
引导握手当前为明文 TCP，必须显式启用；它交换的 endpoint 含 Token。
这版不提供引导身份认证或数据加密，只能用于隔离的实验环境。
AUTH/ACL 在 URMA 数据通道执行；引导 IP 或同 Fabric 不被当成本地可信身份。
需要机密性或受认证引导时，必须等待 TLS 引导及原生数据保护方案。

客户端机器执行：

```sh
# 通过会话环境注入测试凭据，避免写进命令参数/日志。
# VALKEY_URMA_USER 与 VALKEY_URMA_PASSWORD 由测试环境提供。
build/urma-runtime/client-probe 192.0.2.10 6380 <客户端设备名> <EID索引> \
  --allow-insecure-bootstrap
```

探针通过明文 TCP 交换 HELLO、两个 endpoint 和 `RDY1`，之后只从 URMA 传输
AUTH、PING、65537 字节二进制 SET/GET 和 DEL。测试 Key 带随机后缀，正常结束时
删除；异常中断可能遗留测试 Key。命令校验和资源清理均成功才返回 0。
服务端握手后若从引导 TCP 收到业务字节，会报错断开。

还必须在真实机器检查：反复连接/关闭、缓慢读写、maxclients/握手额度耗尽、
CLIENT KILL、进程/链路故障、迟到完成、资源恢复及双端字节内容。
SDK 的同步 import/bind/delete、异步错误确认及 DMA 终止保证仍需 provider 核验；
没有完成这些验证前，不能把本实验分支用于生产或宣布 U1/U4/U5 硬件验收完成。

## 本地软件验证

可从已有 TCP 管理监听执行 `INFO urma`（`INFO all` 也包含该节）。
`urma_compiled` 与 `urma_listener_enabled` 分别表示构建能力和监听配置；
`urma_transports` 包括等待删除的 owner，`urma_retired_transports` 给出退役队列数。
`urma_buffer_used` 是含保守预留的应用缓冲准入费用，不是 RSS，也不包含驱动内存。
`urma_tx_completions/tx_wire_bytes` 只在成功 SDK 完成后计数；RX 在校验通过后计数。
wire 字节包括帧头和控制消息，不是 Value 大小或命令执行确认。
`urma_ready_total`、重试、错误、初始化失败、预算拒绝与超时计数保留至 runtime
销毁，不随 `CONFIG RESETSTAT` 清零。正常关闭不算传输错误；删除失败仍保留额度。

`urma-progress-timeout` 是正整数毫秒，默认 10000，重启生效。它限制已接受发送的
投递/完成等待和明确失败的 receive repost；重试不会续期，空闲 RX 或单纯等待
对端信用不会因此断开。先处理本轮观测到的完成，再检查剩余期限。到期只锁存
逻辑错误，仍保留设备可能引用的内存；也不能打断同步 SDK 调用或证明 DMA 停止。

```sh
make -C src test-unit MALLOC=libc BUILD_URMA=yes UNIT_TEST_PATTERN=Urma
make -C tests/urma test
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  make -C tests/urma test OUT=../../build/urma-runtime-sanitized \
  SANITIZER_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
python3 tests/urma/test_startup.py /absolute/path/to/valkey-server
./runtest --single unit/urma --clients 1
./runtest --single unit/protocol --clients 1
```

自定义 SDK 路径时继续传入对应的 `URMA_CFLAGS`/`URMA_LIBS`；首次 sanitizer
使用独立 OUT，改变编译参数后先清理该 OUT。runtime 测试在 SDK seam 替换函数，
只证明失败处理、额度、信用和重试逻辑，不模拟网络、内核或 DMA。
`test_startup.py` 同时适用于默认构建与真实 SDK 构建，不需要设备。

实现采用 1ms 定时轮询，每连接每轮最多处理 64 个完成；尚未接入硬件事件通知。
配置拒绝多 I/O 线程、自动快照/AOF、主从上行和未验收的集群组合；后台 fork
在启用 URMA 监听时也会被明确拒绝。出站连接、同步 I/O、复制/迁移、fork
兼容与性能验收未完成。资源删除失败时保留缓冲及其额度并重试，不在超时后擅自
释放 DMA 可能使用的内存。额度覆盖应用缓冲，不覆盖 SDK/驱动/内核资源。
