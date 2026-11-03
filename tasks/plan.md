# 原生 URMA 接入与远端内存演进计划

状态：用户已通过“继续”授权本地实现；U2 已完成，U3 的独立流模型已实现，
SDK、ConnectionType 接线及硬件验收待完成。日期：2026-11-03。

## 目标与范围

第一阶段在 Valkey `ConnectionType` 层直接调用 URMA，保持 RESP、AUTH/ACL
及命令执行语义。使用双边收发实现有序字节流，先完成单机服务端、C 客户端、
CLI 与 benchmark 的端到端闭环，再扩展 I/O 线程和内部通信链路。

第二阶段独立研究内存架构改造：让部分缓存和大 String Value 使用池化、
远端内存，逐步进入内存分配、对象生命周期及数据访问层。
原生通信不以远端 Value 实现为前置条件。

工作文档索引：

- [任务及验收清单](todo.md)
- [原生通信设计](urma-architecture.md)
- [远端内存研究路线](remote-memory-roadmap.md)
- [上游讨论草稿](upstream-issue-draft.md)
- [基础模块验证记录](validation-2026-11-03.md)

## 依据与本地检查

- Valkey 基线：`e581052a6007a0e12cdd8808391b5c745c915037`。
- 用户确认目标处理器平台为鲲鹏 950；UB 设备/provider、发行版及内核版本未知。
  鲲鹏 950 不是 Linux 内核版本。
- 用户本地没有测试机；真实设备验证在内网 clone 后进行。
- Windows 本地已有 MinGW GCC、CMake；WSL Ubuntu 26.04.1 中确认有 GCC、
  G++、Make、Python 和 pkg-config，未发现 URMA 头文件及 UB 设备。
  本轮已在 WSL 补齐 CMake、Tcl、clang-format-18、gtest/gmock；
  Make/CMake 的 libc 构建和基础模块测试已执行，具体边界见验证记录。
- 不将 WSL 测试通过等同于 aarch64/鲲鹏 950 或 UB 硬件支持。
- 本地没有既存 `tasks/plan.md`、`tasks/todo.md`；本次不覆盖其他计划。

本地自定义 skills 已检查：任务拆解、接口设计、模块设计、架构记录、代码审查、
完成前验证；本轮源码开发同时应用 cpp-testing、property-based-testing、
address-sanitizer 及 systematic-debugging。测试使用固定种子的动作序列和
编解码循环，不增加属性测试框架依赖。当前不需要并行代理。

## SDK 基线

UMDK 镜像发布说明最新条目为 `v26.06.0`，发布说明日期 2026-06-30。
2026-11-03 查询到的 master 快照为
`7c8a9541cba083c22ffc55596f7937f991eeae1a`（提交时间 2026-11-02）。
该 SHA 是开发参考快照，不声称它是 v26.06.0 的发布标签，也不声称已通过硬件认证。
开发时固定 SHA；内网验收同时记录实际安装包版本、库 SONAME、头文件版本、
驱动、固件、页大小和设备能力。若不匹配，先更新兼容矩阵，再增加明确的版本适配。

- [固定快照发布说明](https://github.com/openeuler-mirror/umdk/blob/7c8a9541cba083c22ffc55596f7937f991eeae1a/RELEASE-NOTES.ch.md)
- [固定快照官方样例](https://github.com/openeuler-mirror/umdk/blob/7c8a9541cba083c22ffc55596f7937f991eeae1a/src/urma/examples/README.md)

## 实施顺序与关口

| 阶段 | 任务 | 进入下一阶段的条件 |
| --- | --- | --- |
| 计划 | P0 | 用户确认本地实现计划；上游接受度另行记录 |
| 平台与协议 | U1–U3 | 固定 SDK 可编译；协议校验及独立探针可复现 |
| 连接闭环 | U4–U8 | 真实双机完成服务端、C 客户端、CLI 的正常及异常路径 |
| 稳定性 | U9–U10 | I/O 线程、fork、故障恢复和资源统计经过验证 |
| 内存研究 | M1–M4 | 单独确认内存故障模型与命令兼容范围 |

无硬件时可以推进协议单元测试、源码构建及状态机验证；U1 的设备验收和后续
所有硬件完成项保持未勾选。不能用 TCP、Soft-RoCE 或伪造 provider 冒充原生 UB。

## Commit 与上游流程

工作分支：`work/urma-native-design`，从已检查的本地基线创建。
使用现有 Git 身份，不修改全局配置。每个 commit 使用 `git commit -s` 添加 DCO。
标题使用英文祈使句，正文说明问题、设计理由、验证及未验证项；不虚构 issue/PR 编号。
仓库没有要求 Conventional Commits，不额外强制 `feat:` 等前缀。

每个 commit 只包含一个可说明的逻辑改动及其测试；重构、功能、性能和内存语义
改造分开提交。默认构建持续可用；可选功能开启后不能只返回成功而没有真实实现。
计划文档的 commit 只声明文档工作，不声明功能已支持。

示例标题：`Document native URMA transport and remote-memory plan`、
`Add URMA frame validation`、`Support URMA client connections`。
实现顺序和具体提交边界见任务清单，后续依实际 diff 进一步拆小。

`CONTRIBUTING.md` 和 `design-docs/README.md` 要求大型特性先在上游 issue 对齐。
目前没有已知的跟踪 issue 或维护者共识，因此本次仅在 `tasks/` 写本地工作文档，
不把它们冒充已批准的上游设计，不改 `design-docs/`。
上游草稿尚未发布；发布、发 PR 或推送前须有相应授权及用户 fork。
当前 origin 是上游 `valkey-io/valkey`，不能向它推送或直接推送 unstable。

## 验证原则

所有代码批次遵守 AGENTS.md：先跑最小相关测试，C/C++ 用 clang-format-18；
gtest 测试使用简洁 C 风格。对协议输入验证及状态机条件做有意义的负向检验。
构建结果、单元测试、SDK 链接、真实设备收发和性能报告分别记录。
内存阶段的 RDB/AOF、复制、脚本、事务、模块接口和升级兼容验证不能省略。

## 待确认事项

1. 内网机器的 OS、实际内核、UB 设备/provider、UMDK/URMA/驱动/固件组合。
2. 引导通道使用 TLS 的部署凭据，以及原生数据通道的机密性要求。
3. C 客户端作为 Valkey 捆绑依赖的配套补丁还是同步到独立 libvalkey 仓库。
4. 内存容量扩展是否要求在远端节点故障后继续提供已确认写入的数据。
