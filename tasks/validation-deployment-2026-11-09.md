# 普通 TCP 部署：本地验证记录

日期：2026-11-09。用户确认先完成本地验证，202/204 按
[现场部署基线](deployment-baseline-2026-11-09.md) 验收。本次未登录这两台主机。

## 结论与范围

同一份干净源码的普通 Make 构建、真实 liburma CMake 构建，均在非 root 用户下
通过独立 TCP 实例的启动、认证、读写、保存及重启恢复测试。两者均显式关闭
URMA/RDMA 监听；SDK 构建的动态库能够加载，不要求 UB 设备初始化。

这证明本地 WSL/x86_64 的普通应用运行路径可用，不证明目标 aarch64/openEuler
环境、跨机网络、现有 ACL 或原生 URMA 已通过。todo.md 的现场 D0–D3 保持未完成。

## 环境、源码和构建

- 执行时间：2026-11-09 11:31 UTC（北京时间 19:31）。
- Linux 用户：UID 1000；没有使用 sudo 或 Linux root。
- 内核：`6.18.54.1-microsoft-standard-WSL2`，架构 `x86_64`。
- 固定源码：`aff2d3350e57e1c222d28743b486a940b0ac992c`，通过 `git archive`
  导出到全新 `/tmp/valkey-deployment-7q926vrt`，不改动工作区或系统安装目录。
- 两套构建完成后，逐文件对比导出归档，全部归档文件一致。
- 测试脚本为本轮新增 `tests/urma/test_tcp_deployment.py`，与被测服务端源码分开记录。
  SHA-256：`f63eb97bb2b8b500109bf3812df14b4318837a290b6fe27f1b77992ae380ccc8`。

| 构建 | 命令/选项 | 结果 |
| --- | --- | --- |
| 普通 Make | `make -j4 MALLOC=libc BUILD_URMA=no` | 退出 0 |
| SDK CMake 配置 | `-DCMAKE_BUILD_TYPE=Release -DBUILD_MALLOC=libc -DBUILD_URMA=ON`，显式指定 SDK include/library | 退出 0 |
| SDK CMake 构建 | `cmake --build <tree>/cmake-sdk -j4` | 退出 0 |

SDK 仍使用此前固定快照的 core/common：
`7c8a9541cba083c22ffc55596f7937f991eeae1a`。
头文件为 `/tmp/valkey-urma-sdk/src/urma/lib/urma/core/include`，库为
`/tmp/valkey-urma-sdk-cmake/src/urma/lib/urma/core/liburma.so`。
`ldd` 均无 `not found`；SDK 构建解析到 `liburma.so.0` 与 `liburma_common.so.0`。
普通构建不依赖这两项库。本次未安装 provider、驱动或变更内核。

归档构建的 `--version` 显示 `sha=00000000:1`，不能用该字段证明提交来源；
本记录以固定归档 SHA、归档文件比较及二进制 SHA-256 建立对应关系：

- 普通服务端：`3c04d27592648b75d482c71f560d029956cea9e71bdbb3e1a9592b683f3cc940`。
- SDK 服务端：`f8d2b2b9f6f21b7744773d4b8dc2eb76db0f44a625587e7f3f1a9862d4b3c928`。

最初检查发现此前 SDK 构建目录含与当前归档不同的文件，因此未采用该目录作为
本次提交验证依据；最终两套结果均来自上述新导出的同一源码树。

## 执行与结果

从工作区调用新增脚本，传入构建产物的绝对路径：

```bash
tree=/tmp/valkey-deployment-7q926vrt
python3 /mnt/d/valkey/valkey/tests/urma/test_tcp_deployment.py \
  "$tree/src/valkey-server" "$tree/src/valkey-cli"
python3 /mnt/d/valkey/valkey/tests/urma/test_tcp_deployment.py \
  "$tree/cmake-sdk/bin/valkey-server" "$tree/cmake-sdk/bin/valkey-cli"
```

| 检查 | 普通构建 | SDK 构建 |
| --- | --- | --- |
| 二进制版本与动态库可加载 | 通过 | 通过 |
| 私有目录/ACL、回环端口、本人子进程 PID 匹配 | 通过 | 通过 |
| 无凭据 NOAUTH、错误凭据 WRONGPASS；正确凭据访问 | 通过 | 通过 |
| 文本 SET/GET/DEL、空值、65540 字节二进制逐字节读回 | 通过 | 通过 |
| 十次新建 TCP 连接 PING | 通过 | 通过 |
| SAVE 返回 OK，生成非空 RDB | 通过 | 通过 |
| 停止本人实例，启动新进程，run_id 改变且二进制值精确恢复 | 通过 | 通过 |
| 两个子进程正常退出，自己的临时目录已删除 | 通过 | 通过 |
| 脚本退出码 | 0 | 0 |

两次启动的 `INFO urma` 都要求以下值为 0：`urma_listener_enabled`、
`urma_transports`、`urma_buffer_used`、`urma_initialization_failures`。
普通构建 `urma_compiled=0`，SDK 构建 `urma_compiled=1`。

本地原始证据保留在忽略的 `build/deployment-check/`：`make.log`、
`cmake-sdk-0.log`、`cmake-sdk-1.log`、`default-tcp.log`、`sdk-tcp.log`、
`source-sha.txt` 和 `evidence.json`。这些运行产物不提交；本文件保留可共享的检查结果。

提交前另做 Python 语法、24 个本地文档链接和 8 段部署 Bash 示例语法检查，均通过。
审查时仅在临时脚本副本反转 PID 归属判断，运行按预期失败并停止本人子进程；
原脚本未变。该负向检查证明 PID 判断实际被执行，不声称覆盖所有故障场景。

## 后续关口

1. 现场 D0–D3：核对版本/库/权限，204 独立 TCP 启动，202 认证读写，保存后重启恢复。
2. 平台组合归档并定位原生 URMA 的 OT 能力拒绝原因；不得仅删除能力检查使启动通过。
3. 原生 URMA 双机功能、异常恢复、观测指标与资源维护证据验收。
4. 完成原生客户端/CLI 配套，再将可重建缓存接入实际命令，覆盖失效、过期和本地回退。

后续仍保留本地权威副本；M2–M4 的权威远端容量层、映射池及 U9/U10 的
并发持久化/复制/集群是独立工作，不构成本轮普通 TCP 部署的前置依赖。
