# liburma runtime 与 ConnectionType 软件验证

日期：2026-11-03。承接 [基础模块记录](validation-2026-11-03.md)。
用户已授权推送到 `fengjiuko/valkey-ub` 并继续；现有基础提交已推送工作分支，
本轮源码按 Session、runtime、构建、配置、adapter 和探针分别提交，均带 DCO。
主要代码提交：runtime `dfb1d0a5b`、adapter `badfe7c90`、fork 门禁 `b56b7b814`、
内网工具 `a05e20655`。这些工作仍在实验分支，没有创建上游 PR。

## 实现范围

- `urma_transport.[ch]` 直接调用真实 liburma，不引入生产 provider 接口。
  创建 Context/JFCE/JFC、共享 JFR、两个 Jetty 和本地 Segment；按 RC/RTP/OT
  检查原生 UB 能力，不把可靠性与任意模式下的顺序保证混同。
- DATA/CLOSE 共用有序通道；CREDIT 有独立发送槽和两个接收槽，累计值合并。
  DATA 接收槽数另留一个位置给 CLOSE；每个发送请求要求本地完成记录。
- 应用缓冲在创建硬件资源前计入全局额度；失败删除保留整个对象和额度，
  后续重试。释放前必须成功删除端点/接收队列并注销 Segment。
  只有 SDK 明确标记单条 WR 未接受时才重试，提交不确定时中止并保留缓冲。
- `urma.c` 注册实验性入站 ConnectionType，接入 read/write/writev、短读写、
  pending data 和 connhelpers。握手上限 64、超时 5 秒；maxclients 在创建
  transport 前沿用既有准入。接受回调在成功或失败时取出并清空。
- 明文引导为 HELLO/endpoint/RDY1；之后 TCP 只承担连接存活检测，业务字节
  必须走 URMA。默认关闭，启用明文引导必须显式设置配置；AUTH/ACL 沿用
  Valkey 路径，locality 保守返回非本地。暂不支持硬件中断、出站或同步 I/O。

## SDK 与构建证据

固定参考 SHA：`7c8a9541cba083c22ffc55596f7937f991eeae1a`。
通过 GitHub connector 取得 core/common 的 41 个原始文件，并逐一核对 Git blob SHA；
文件保存在忽略的 build 目录，没有提交 SDK 副本。
使用原始 core/common CMake target 加最小顶层 wrapper 构建 `liburma.so.0.0.3`
（SONAME `liburma.so.0`）。没有构建 provider，也没有安装完整 SDK 包。

平台为 WSL Ubuntu 26.04.1，x86_64，实际内核
`6.18.54.1-microsoft-standard-WSL2`，页大小 4096。测试机器不是鲲鹏或 UB 平台。
代码通过 Git archive 的 LF Linux 临时树构建，避免 Windows CRLF 脚本问题。

| 检查 | 结果与边界 |
| --- | --- |
| 默认 Make libc 构建 | 通过；没有 SDK include/link 依赖 |
| Make `BUILD_URMA=yes` | 真实头文件/共享库编译链接通过 |
| CMake `BUILD_URMA=ON` | server、CLI、benchmark 和原生 gtest 目标构建通过 |
| SDK probe `--link-only` | 通过；不执行设备收发 |
| SDK probe `--inventory` | init=0、设备数=0、uninit=0、退出 1，属于预期拒绝 |
| Make 原生 `UNIT_TEST_PATTERN=Urma` | 24 项通过：协议 10、Session 13、fork 拒绝 1 |
| CMake 原生 gtest，`Urma*` | 同样 24 项通过；使用系统 gtest/gmock 1.17 源码 |
| 基础模块独立 ASan/UBSan/泄漏检查 | 23 项通过；原有两项变异仍被检出 |
| SDK seam runtime 测试 | 6 项通过，ASan/UBSan/泄漏检查通过 |
| runtime 变异 | CREDIT 错投数据通道、模糊提交允许重试，均被测试检出 |
| 默认 Make、真实 SDK Make/CMake 启动脚本 | 四类配置拒绝与默认 TCP PING 均通过 |
| Tcl `unit/protocol` | 37 项通过，0 失败 |
| Tcl `unit/urma` | 3 项通过，默认值、不可变配置及普通 RESP |

SDK seam 测试覆盖七个资源分配失败位置、预算提前拒绝、删除失败后保留与退役回收、
非法 endpoint、bind 失败、错误完成、不确定提交，以及 64 轮短写/顺序/信用/关闭。
这些替换函数仅链接测试二进制；真实 server 与客户端探针链接真实 liburma。
本地临时变异只修改副本，原始源码再次通过；不将它们提交为产品或模拟硬件。

## 内网复现与剩余关口

可复现命令和双机步骤见 [tests/urma/README.md](../tests/urma/README.md)。
它提供 SDK inventory、启动拒绝检查、sanitizer 入口及原生客户端探针；
后者校验 PING 和 65537 字节二进制 SET/GET/DEL，未在本地执行真实通信。

尚无真实 UB 收发、实际 provider 模式组合验证、DMA 终止、异步错误确认或
同步 import/bind/delete 的时间上界证据。未完成硬件中断接入、完整传输超时、
adapter 的双机生命周期/回调验收、libvalkey/CLI/benchmark 原生选项、I/O 线程、
fork、复制/集群、性能或远端 Value。代码禁止子进程进行 runtime 操作，不能据此
声称 fork 已兼容。启用 URMA 时拒绝自动快照/AOF/主从上行，后台 fork 直接报错；
这些限制不是持久化兼容实现。需要机密性或认证引导时，明文实验方案不能满足要求。

U1 的 SDK 核心编译完成；U4/U5 源码路径已展开，但硬件验收及 Checkpoint A 保持
未完成。内网必须先盘点并运行官方样例，再用专项探针验证，不能以本记录替代。
