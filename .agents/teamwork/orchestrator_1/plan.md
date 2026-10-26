# Plan — Networked GPU Offloading Project

## Overview
Build a high-performance networked GPU offloading system that intercepts Direct3D 11/12 graphics API calls on a client Windows PC, serializes commands over a network socket, executes rendering on a remote host GPU, and streams back rendered frames for presentation on the client.

## Phases

### Phase 0: Survey & Specification Extraction
- Spawn 3 parallel agents:
  1. `teamwork_preview_spec_miner` (Spec Miner 1): Analyze requirements R1-R5, acceptance criteria, Direct3D 11/12 COM hooking/proxying strategies, entrypoints (d3d11.dll, dxgi.dll).
  2. `teamwork_preview_explorer` (Explorer 1): Investigate network transport, packet serialization/deserialization, memory layout, flow control, handshake, and error handling.
  3. `teamwork_preview_explorer` (Explorer 2): Investigate remote GPU execution server architecture, DXGI adapter enumeration, offscreen render target setup, framebuffer readback (Staging Texture / CopySubresourceRegion), client presentation swapchain, and automated headless test harness design.

### Phase 1: Architecture & Project Definition
- Aggregate survey reports.
- Produce `PROJECT.md` at project root with:
  - Architecture and module boundaries
  - Complete Feature Inventory
  - Milestone definitions
  - Interface contracts
  - Code layout
  - Build and run commands

### Phase 2: Milestone Iteration Loops (Dual Track)
- For each milestone:
  - Explorer(s) analyze and recommend approach.
  - Worker implements code and unit tests.
  - Reviewer(s) perform code review and verify builds/tests.
  - Challenger(s) run empirical tests and stress checks.
  - Forensic Auditor performs integrity forensics.
  - Gate evaluation: strict AND of all criteria.

### Phase 3: Final E2E Verification & Sentinel Victory Claim
- Execute automated loopback test suite: 60+ rendered frames, non-empty/non-zero luminance pixel verification, clean returncode 0.
- Ensure all acceptance criteria are met.
- Report completion and send victory claim to Sentinel.
