# VMX-SENTINEL: Kernel Virtualization Anti-Cheat Arena


## Official Sandbox for Hypervisor-Level Memory Integrity Research  
*Red Team (Attackers) vs Blue Team (Defenders) | Expert Analysis by QKV*

---

## Overview

VMX-SENTINEL is a specialized Windows kernel-mode sandbox designed for advanced virtualization-based anti-cheat research. This project creates a realistic FPS game environment with precise memory layout and physics simulation to serve as a battleground for hypervisor-level attack and defense techniques.

The sandbox implements authentic game mechanics while maintaining a rigorously defined 80-byte memory structure that serves as the primary target for memory manipulation attempts. Unlike traditional anti-cheat solutions, VMX-SENTINEL focuses exclusively on the hypervisor layer (VMX root mode) for protection, representing the cutting edge of game security architecture as of 2026.

## Key Features

- **Precision Memory Layout**: Exactly 80-byte `PlayerState` structure with field-aligned offsets optimized for cache performance
- **Authentic FPS Physics**: Human-limited aiming mechanics, parabolic jump trajectories, and realistic ammo management
- **Zero-Flicker Console Interface**: Double-buffered rendering with perfect window sizing for analysis
- **Session Rotation**: Automatic 30-second session ID rotation to prevent static memory targeting
- **System-Level Protections**: Windows process mitigation policies including dynamic code prohibition and binary signature enforcement
- **Memory Analysis Ready**: Complete offset mapping and addressing information exposed for research purposes
- **Copy-Enabled Interface**: Text selection and copying capabilities for analysis reporting

## Technical Specifications

| Component | Specification |
|-----------|---------------|
| **Target Platform** | Windows 10/11 x64 (Build 22621+) |
| **Required Privileges** | Standard user (no admin required) |
| **Memory Layout** | Strict 80-byte aligned structure (cache line optimized) |
| **Timing System** | QPC high-resolution timestamps (hardware backed) |
| **Rendering** | Double-buffered console with atomic updates |
| **Window Dimensions** | 120×40 characters (fixed, no scrollbars) |
| **Font** | Lucida Console (system default) |
| **Physics Accuracy** | 50ms frame time with human biomechanical limits |

## Memory Layout (Critical Analysis Target)

```
Offset  Size  Type       Field           Description
------  ----  ----       -----           -----------
0       4     DWORD      sessionId       30-second rotating identifier
4       4     LONG       score           Interlocked increment counter
8       4     FLOAT      health          100.0 → 70.0 → recovery cycle
12      16    CHAR[16]   playerName      Static player identifier
28      12    Vector3    position        X/Y/Z coordinates (SIMD aligned)
40      4     FLOAT      pitch           -90.0° to 90.0° (vertical aim)
44      4     FLOAT      yaw             -180.0° to 180.0° (horizontal aim)
48      4     DWORD      currentClip     Magazine ammunition (0-30)
52      4     DWORD      reserveAmmo     Reserve ammunition (0-200)
56      8     ULONGLONG  lastUpdate      QPC timestamp of last update
64      8     ULONGLONG  lastAmmoRefill  QPC timestamp for ammo cycle
72      8     ULONGLONG  lastHealthEvent QPC timestamp for health cycle
```

## Competition Roles

- **🔴 Red Team (G老师)**: Develop hypervisor-level attacks to manipulate game state from VMX root mode  
- **🔵 Blue Team (轻子)**: Create detection mechanisms for unauthorized memory access and hypervisor tampering  
- **⚫ Expert Team (QKV)**: Provide architectural analysis and advanced technique evaluation

## Build Instructions

```bash
# Prerequisites
# - Visual Studio 2022 (v17.8+)
# - Windows SDK 10.0.22621.0+
# - x64 Native Tools Command Prompt

# Compilation
cl /EHsc /O2 /std:c++17 /fp:fast main.cpp /link /subsystem:console

# Output
# - VMX-Sentinel.exe (VMX-SENTINEL executable)
```

## Usage

1. Execute `VMX-Sentinel.exe` to launch the sandbox environment
2. The console interface will automatically size to 120×40 characters
3. All memory addresses and offsets are displayed for analysis purposes
4. Press `ESC` to terminate the application gracefully
5. Memory analysis tools can attach to the process using the displayed PID

## Research Value

VMX-SENTINEL represents the state-of-the-art in 2077 game protection architecture by:

1. **Isolating game logic from detection**: Pure game mechanics without embedded anti-cheat code
2. **Providing clear memory targets**: Well-defined structure with documented offsets
3. **Simulating authentic player behavior**: Human-limited inputs prevent false positive detection
4. **Enforcing hardware boundaries**: Leveraging Windows mitigation policies as baseline protection
5. **Enabling precise measurement**: High-resolution timing for attack detection analysis

This sandbox specifically focuses on the hypervisor attack surface, where modern anti-cheat systems like EAC, BattleEye, and Tencent ACE implement their most sophisticated protections.

---

**Disclaimer**: VMX-SENTINEL is strictly for security research and educational purposes. This software must only be used in authorized testing environments with explicit permission from all relevant parties. Unauthorized use against production systems violates multiple international laws and treaties.

**VMX-SENTINEL** | Kernel Virtualization Arena 2077 | (C) QKV Research Group