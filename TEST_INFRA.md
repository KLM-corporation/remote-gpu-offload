# E2E Test Infra: Networked GPU Offloading

## Test Philosophy
- Opaque-box, requirement-driven, zero dependency on internal implementation design.
- Methodology: Category-Partition + Boundary Value Analysis + Pairwise Combinatorial + Real-World Workload Testing.
- Rigorous automated verification without human intervention.

## Feature Inventory Coverage Matrix
| # | Feature | Source (Requirement) | Tier 1 | Tier 2 | Tier 3 | Tier 4 |
|---|---------|---------------------|:------:|:------:|:------:|:------:|
| 1 | Socket Transport & Framing | ORIGINAL_REQUEST §R2 | 5 | 5 | ✓ | ✓ |
| 2 | Packet Serialization | ORIGINAL_REQUEST §R2 | 5 | 5 | ✓ | ✓ |
| 3 | Handshake & Lifecycle | ORIGINAL_REQUEST §R2 | 5 | 5 | ✓ | ✓ |
| 4 | Network Fault Tolerance | ORIGINAL_REQUEST §R2 | 5 | 5 | ✓ | ✓ |
| 5 | DXGI Adapter Selection | ORIGINAL_REQUEST §R3 | 5 | 5 | ✓ | ✓ |
| 6 | Headless D3D11 Device | ORIGINAL_REQUEST §R3 | 5 | 5 | ✓ | ✓ |
| 7 | Host GPU Resource Manager | ORIGINAL_REQUEST §R3 | 5 | 5 | ✓ | ✓ |
| 8 | Server Command Execution | ORIGINAL_REQUEST §R3 | 5 | 5 | ✓ | ✓ |
| 9 | Framebuffer Readback | ORIGINAL_REQUEST §R4 | 5 | 5 | ✓ | ✓ |
| 10 | Client Interception DLLs | ORIGINAL_REQUEST §R1 | 5 | 5 | ✓ | ✓ |
| 11 | COM Proxy Object Machine | ORIGINAL_REQUEST §R1 | 5 | 5 | ✓ | ✓ |
| 12 | Dynamic Buffer Mapping | ORIGINAL_REQUEST §R1 | 5 | 5 | ✓ | ✓ |
| 13 | Zero-Hardware Presentation | ORIGINAL_REQUEST §R4 | 5 | 5 | ✓ | ✓ |
| 14 | Frame Flow Control | ORIGINAL_REQUEST §R2, §R4 | 5 | 5 | ✓ | ✓ |
| 15 | Standalone 3D Sample App | ORIGINAL_REQUEST §R5 | 5 | 5 | ✓ | ✓ |
| 16 | Loopback Test Runner | ORIGINAL_REQUEST §R5 | 5 | 5 | ✓ | ✓ |
| 17 | Programmatic Pixel Verifier | ORIGINAL_REQUEST §R5 | 5 | 5 | ✓ | ✓ |
| 18 | E2E System Integration | Acceptance Criteria | 5 | 5 | ✓ | ✓ |

## Test Architecture
- **Test Runner**: `scripts/run_loopback_test.py`
  - Launches server in background on loopback `127.0.0.1:48888`.
  - Launches client sample application with drop-in `d3d11.dll`.
  - Synchronizes frame rendering for >= 60 frames.
  - Captures rendered framebuffers from readback stream.
  - Invokes `tests/verify_pixels.py` on captured framebuffer.
  - Enforces clean exit code 0 for both processes.
- **Pass / Fail Semantics**:
  - Exit code 0 on all components.
  - Zero packet desynchronization or socket drops.
  - Pixel luminance mean $\in [10, 245]$.
  - Pixel luminance standard deviation $\ge 3.0$.
  - Non-clear background pixel ratio $\ge 2.0\%$.
  - Color cluster variety $\ge 2$.

## Coverage Thresholds
- Tier 1 (Feature Coverage): $\ge 5$ test assertions per feature.
- Tier 2 (Boundary & Corner Cases): empty packets, oversized packets, disconnect during draw, window resize, zero-sized buffers.
- Tier 3 (Cross-Feature Combinations): dynamic constant buffer update + texture upload + draw indexed.
- Tier 4 (Real-World Application Scenarios): 60+ continuous animated frames of textured 3D mesh over loopback socket.
