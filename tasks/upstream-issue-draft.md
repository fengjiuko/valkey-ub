# Upstream discussion draft: Native URMA connections for Valkey

Status: local draft, not published. No upstream issue number or maintainer approval
is claimed. User review of a local plan is separate from upstream acceptance.

## Problem and use case

Valkey has an experimental Verbs/RDMA transport. A deployment targeting a Kunpeng
950 platform needs to evaluate direct URMA communication over UB, with matching C
client support, without changing RESP or command execution semantics.

## Proposed scope

- Add an optional Linux ConnectionType backed by liburma.
- Start with bounded two-sided messaging and explicit stream/flow-control semantics.
- Reuse admission control, AUTH/ACL and connection lifecycle rules.
- Provide matching libvalkey, CLI and benchmark support.
- Keep TCP/TLS bootstrap separate from URMA business traffic.
- Leave replication/cluster transport changes and remote Value storage to separate
  proposals and validation gates.

## Questions for maintainers

1. Is a native URMA transport a conceptual fit, and which upstream tracking/design
   workflow should it follow?
2. What is the preferred ownership of shared protocol definitions and client code?
3. Which dependency, supported-platform and hardware-CI commitments are required?

## Evidence available and remaining

Local source review and a reproducible plan are available. The SDK reference is
pinned in [plan.md](plan.md). The target UB provider, kernel/driver/firmware matrix
and hardware results must be supplied from the internal test environment.
There is no verified native URMA implementation or performance claim yet.
