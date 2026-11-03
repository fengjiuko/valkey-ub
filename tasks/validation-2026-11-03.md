# URMA 基础模块验证记录

日期：2026-11-03。源码提交：`f20127470`（协议）、`4ec3745ba`（会话模型）。
分支：`work/urma-native-design`。这两批改动分别为五个文件，并带 DCO 签名。

## 验证范围与环境

Windows 工作区的源码在 WSL Ubuntu 26.04.1、x86_64 上验证，使用 GCC/G++、
Make、CMake、Tcl、GoogleTest 1.17 及 clang-format 18.1.8，分配器为 libc。
此处没有 liburma 头文件、库或 UB 设备，未执行 SDK 链接或设备收发测试。
这份记录不能证明鲲鹏 950、aarch64 或任一 UB provider 的兼容性。

Windows checkout 使用 CRLF，直接在 `/mnt/d` 构建时脚本 shebang 导致
`mkreleasehdr.sh` 启动失败。本轮采用 Git archive 产生 LF 的 Linux 临时树，
再覆盖本轮源文件进行构建，未整体修改仓库换行或提交生成文件。
内网使用正常 Linux clone 时可直接执行下面的仓库命令。

## 命令与结果

在仓库根目录运行；需要系统 GCC/G++、Make、CMake、Python 3、Tcl 和 gtest/gmock。
以下普通构建不需要 URMA SDK，也不会启用 URMA 监听。

```sh
make -j4 MALLOC=libc
make -C src -j4 test-unit MALLOC=libc UNIT_TEST_PATTERN=Urma
./runtest --single unit/protocol --clients 1
cmake -S . -B build/cmake-urma -DBUILD_MALLOC=libc -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cmake-urma --parallel 4
python3 tasks/check-urma-foundation.py
clang-format-18 --dry-run --Werror src/urma_protocol.[ch] src/urma_session.[ch] \
  src/unit/test_urma_protocol.cpp src/unit/test_urma_session.cpp
git diff --check
```

| 检查 | 结果 |
| --- | --- |
| Make libc 构建 | 通过，包含 server/CLI/benchmark |
| CMake Debug libc 构建 | 通过，包含 server/CLI/benchmark |
| 原生 gtest 入口，过滤 Urma | 22 项通过：协议 10，会话 12 |
| Tcl `unit/protocol` | 37 项通过，0 失败 |
| 独立 ASan + UBSan + LeakSanitizer | 同一组 22 项通过，未报告错误 |
| 去掉 DATA payload 长度上限的临时变体 | 协议测试失败，检出预期错误 |
| 将已读完槽位提前视为已重新投递的临时变体 | 专项测试失败，检出预期错误 |
| 原始源码重跑 | 22 项通过；变异使用副本，未修改生产源码 |
| clang-format-18、diff 空白检查 | 通过 |

[检查脚本](check-urma-foundation.py) 仅链接协议、会话、真实 zmalloc 和断言实现，
使用现有 gtest；不是 provider fake。脚本在忽略的 `build/urma-foundation/`
生成二进制与日志，以 `-fsanitize=address,undefined` 编译，启用泄漏检查和错误即停。
变异仅在临时副本上进行，要求测试以失败状态退出，再验证原始版本通过。
它补充仓库原生测试入口，不替代完整 Valkey 或硬件测试。

## 覆盖与剩余工作

协议覆盖固定 wire 向量、非对齐输入、截断/尾随数据、非法类型/版本/保留字段、
长度与整数边界、HELLO 能力协商、重叠 payload，以及 512 轮确定性编解码往返。
会话覆盖连续前缀写入、短读、跨帧读取、累计信用去重、投递后归还信用、
乱序/重复/错误代次完成、缓冲借用、失败关闭、正常 EOF 和 1024 轮槽位复用。

尚未实现 ConnectionType 注册、liburma runtime、引导端点交换、控制资源预留、
全局内存准入、事件/超时、CLI/客户端接线、I/O 线程/fork 兼容或远端 Value。
没有开展吞吐或延迟比较；ASan 通过不代表 DMA 已安全终止。
下一阶段先补 U1 的 SDK 编译与平台探针，再实现 U4/U5；内网双机验收仍为必选项。
