# 可重建缓存与 Value 池软件验证

日期：2026-11-06。用户选择保留本地权威副本。实现契约见
[M1 设计](remote-cache-design.md)。此记录仅覆盖独立实验缓存，不是数据库命令
接入、真实 UB 收发或池化远端内存映射的验收报告。

## 实现

- `src/remote_value.[ch]`：caller-owned CPU arena、有界固定 slot、不可回绕 generation、
  epoch/身份/版本/长度/CRC64 校验、单调 TTL、view pin、撤销与延迟复用、独立统计。
- `src/remote_cache.[ch]`：88 字节 v1 wire、长度预检、PUT/GET/DROP 分派和权威端
  当前身份/版本回复发布门禁。超时/错版本/损坏视为 miss，输出不变。
- `tests/urma/cache_probe.c`：固定一客户端四请求的 native URMA 双端探针；
  本地权威缓冲一直保留，remote pool 仅存副本。引导仍需显式允许明文，不能部署
  到未获准的共享/非隔离网络。
- 共用探针 I/O helper，不执行 root 操作或改动设备；软件测试只写本任务 build/
  与 Linux 临时源码目录。真实数据探针未在本地运行。

## 本次证据

环境：WSL Ubuntu 26.04.1、x86_64、普通账户 UID 1000；内核
`6.18.54.1-microsoft-standard-WSL2`。复用原先校验的 SDK 快照
`7c8a9541cba083c22ffc55596f7937f991eeae1a` 的 core 头文件与真实
`liburma.so.0.0.3`，没有 provider 或 UB 设备。

| 验证 | 实际结果 |
| --- | --- |
| 独立 pool/codec gtest | 15 项通过（Value 池 8、缓存 7） |
| ASan/UBSan + `detect_leaks=1` | 同一 15 项通过，无报告 |
| 删除当前版本发布门禁的变异 | 对应迟到回复测试失败，原版重跑通过 |
| 删除缓存内容 CRC 检查的变异 | 对应损坏/重建测试失败，原版重跑通过 |
| 允许复用仍被 pin 的 slot 的变异 | 对应引用/回收测试失败，原版重跑通过 |
| 原生 Make gtest 接线 | `UNIT_TEST_PATTERN=Remote`，15 项通过 |
| 默认 Make libc 构建 | 通过，不需要 URMA SDK |
| `BUILD_URMA=ON` CMake 构建 | server/CLI/benchmark 全部编译链接通过 |
| 真实 SDK 三个探针构建 | sdk/client/cache-probe 全部编译链接通过 |
| SDK seam runtime 测试 | 14 项 ASan/UBSan 通过，其中新增 1 项跨 bounded stream 的缓存链路测试 |
| cache-probe `--help` | 退出 0，不初始化设备 |
| SDK probe `--link-only` | 退出 0；明确没有初始化或测试硬件 |
| `clang-format-18`、diff 检查 | 修改的 C/C++ 已格式化；`git diff --check` 通过 |

15 项缓存测试覆盖：二进制与空值、池满、非法 TTL/尺寸/校验、时钟加法溢出、
身份/版本/epoch/slot generation、pin 后过期/删除、缓存损坏/重建、池重启、
4096 次 extent 复用，以及 wire 截断/尾随/非法保留字段和大消息。
SDK seam 集成测试用两条有界流传递 65537 字节缓存请求与回复，比较本地权威
内容、MISS/hit、释放后的零 live/pin 与 runtime 资源回收。它只替换测试二进制的
SDK 函数，不证明网络、设备、异步硬件错误或 DMA 行为。

复现入口：

```sh
python3 tasks/check-remote-cache.py
make -C tests/urma test
make -C src test-unit MALLOC=libc BUILD_URMA=no UNIT_TEST_PATTERN=Remote
```

SDK seam/probe 构建需传实际 `URMA_CFLAGS`/`URMA_LIBS`，见
[内网说明](../tests/urma/README.md)。普通/消毒器/变异日志存于忽略的
`build/remote-cache-check/`，没有提交本机路径清单或原始日志。

## 剩余关口

本轮没有接入 Valkey 的 GET/SET、Key 失效通知、后台取回调度或本地 fallback
执行路径；当前身份/版本由独立调用者提供。真实 Key 覆盖/删除/过期/重建的
集成证据必须在命令适配器实现后补齐，不以“传入不同身份”的模块测试代替。

尚需真实双机验证 provider/驱动组合、数据和关闭、故障下资源回收及性能收益。
服务端 pool 是进程拥有的可寻址 arena；没有实现平台 CPU 映射、单边 URMA
远端内存操作或权威容量扩展。M2/M3/M4 与 Checkpoint A 未验收项继续保持未完成。

## 审查

按正确性、接口/所有权、协议输入、内存预算及性能边界进行自查；三项有意义的
负向变异被检出。模块没有改动既有数据库对象语义；公共指针仍是本地有效内存。
池与协议、probe helper、native 验证及文档按逻辑提交，所有提交使用 DCO。
