// main.cpp - VMX-SENTINEL SEH COMPATIBILITY FIX
// QKV Expert.
// Compile with: cl /EHsc /O2 /std:c++17 /fp:fast main.cpp /link /subsystem:console

#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <iomanip>
#include <intrin.h>
#include <processthreadsapi.h>
#include <math.h>
#include <string.h>
#include <random>
#include <memory>

typedef BOOL(WINAPI* PSET_PROCESS_MITIGATION_POLICY)(
    PROCESS_MITIGATION_POLICY Policy,
    PVOID pBuffer,
    SIZE_T BufferSize
    );

// ======================
// FPS PHYSICS CONSTANTS
// ======================
constexpr FLOAT MAX_YAW_PER_SECOND = 120.0f;   // Human wrist rotation limit
constexpr FLOAT MAX_PITCH_ACCEL = 300.0f;      // Max pitch acceleration (deg/s²)
constexpr FLOAT JUMP_PITCH_OFFSET = 30.0f;     // Pitch change during jump
constexpr DWORD RELOAD_DURATION_MS = 300;      // Reload animation time

#pragma pack(push, 1)
struct Vector3 {
    FLOAT x;
    FLOAT y;
    FLOAT z;
};

// PRECISE 80-BYTE LAYOUT (FPS PHYSICS OPTIMIZED)
struct PlayerState {
    // Core identity section [0-27]
    DWORD sessionId;          // 0-3
    volatile LONG score;      // 4-7
    FLOAT health;             // 8-11
    CHAR playerName[16];      // 12-27 (16 bytes)

    // Position & orientation section [28-47] (SIMD ALIGNED)
    Vector3 position;         // 28-39 (12 bytes)
    FLOAT pitch;              // 40-43 (-90.0 to 90.0 degrees)
    FLOAT yaw;                // 44-47 (-180.0 to 180.0 degrees)

    // Combat section [48-55]
    DWORD currentAmmo;        // 48-51 (0-30)
    DWORD reserveAmmo;        // 52-55 (0-200)

    // Integrity section [56-79]
    ULONGLONG lastUpdate;     // 56-63 (QPC timestamp)
    ULONGLONG lastPositionUpdate; // 64-71
    BYTE checksum;            // 72
    BYTE canary;              // 73 (anti-overflow sentinel)
    WORD securityFlags;       // 74-75 (future expansion)
    DWORD padding;            // 76-79 (EXACT 80-BYTE ALIGNMENT)
};
static_assert(sizeof(PlayerState) == 80, "CRITICAL: PlayerState MUST be exactly 80 bytes");
#pragma pack(pop)

// ======================
// GLOBAL STATE & CONSTANTS
// ======================
std::atomic<bool> g_exitRequested = false;
HANDLE g_gameMutex = nullptr;
PlayerState* g_playerState = nullptr;
ULONGLONG g_qpcFreq = 0;
PSET_PROCESS_MITIGATION_POLICY g_pSetProcessMitigationPolicy = nullptr;
std::mt19937 g_rng; // Random number generator for physics

const DWORD DEBUG_TRIP_ADDRESSES[] = {
    0x7FFE0000,  // KUSER_SHARED_DATA
    0x7FFD0000,  // Alternate debug region
    0x7FFE0300   // System time fields
};
const size_t DEBUG_TRIP_COUNT = _countof(DEBUG_TRIP_ADDRESSES);

// ======================
// SAFE MEMORY ACCESS UTILITY (SEH COMPATIBLE)
// ======================
inline bool SafeReadByte(const volatile void* address, BYTE& value) {
    __try {
        value = *(reinterpret_cast<const volatile BYTE*>(address));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ======================
// SECURITY ENHANCEMENTS
// ======================
void ApplyProcessMitigations() {
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) return;

    g_pSetProcessMitigationPolicy = (PSET_PROCESS_MITIGATION_POLICY)
        GetProcAddress(hKernel32, "SetProcessMitigationPolicy");

    if (!g_pSetProcessMitigationPolicy) {
        std::cerr << "[SECURITY] SetProcessMitigationPolicy unavailable\n";
        return;
    }

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicPolicy = { 0 };
    dynamicPolicy.ProhibitDynamicCode = TRUE;
    g_pSetProcessMitigationPolicy(ProcessDynamicCodePolicy, &dynamicPolicy, sizeof(dynamicPolicy));

    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sigPolicy = { 0 };
    sigPolicy.MicrosoftSignedOnly = TRUE;
    g_pSetProcessMitigationPolicy(ProcessSignaturePolicy, &sigPolicy, sizeof(sigPolicy));

    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY handlePolicy = { 0 };
    handlePolicy.RaiseExceptionOnInvalidHandleReference = TRUE;
    handlePolicy.HandleExceptionsPermanentlyEnabled = TRUE;
    g_pSetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &handlePolicy, sizeof(handlePolicy));

    std::cout << "[SECURITY] All mitigations active\n";
}

// ======================
// CORE GAME LOGIC (FPS PHYSICS OPTIMIZED)
// ======================
ULONGLONG GetPreciseTime() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

void CalculateChecksum(PlayerState* state) {
    BYTE* bytes = reinterpret_cast<BYTE*>(state);
    BYTE sum = 0;

    for (size_t i = 0; i < sizeof(PlayerState); ++i) {
        if (i >= 4 && i <= 7) continue;  // Skip score field
        if (i == 72) continue;            // Skip checksum field
        sum += bytes[i];
    }
    state->checksum = sum;
}

void NormalizeYaw(FLOAT& yaw) {
    // Normalize to [-180, 180] range
    yaw = fmodf(yaw, 360.0f);
    if (yaw > 180.0f) yaw -= 360.0f;
    if (yaw < -180.0f) yaw += 360.0f;
}

void NormalizePitch(FLOAT& pitch) {
    // Clamp to [-90, 90] range
    pitch = fmaxf(-90.0f, fminf(90.0f, pitch));
}

FLOAT CalculatePositionDistance(const Vector3& a, const Vector3& b) {
    return sqrtf(
        (a.x - b.x) * (a.x - b.x) +
        (a.y - b.y) * (a.y - b.y) +
        (a.z - b.z) * (a.z - b.z)
    );
}

void UpdateGameState() {
    WaitForSingleObject(g_gameMutex, INFINITE);

    // Session rotation (30-second cycle)
    static DWORD sessionEpoch = 0;
    if (GetPreciseTime() - sessionEpoch > g_qpcFreq * 30) {
        g_playerState->sessionId = GetTickCount64() & 0xFFFFFFFF;
        sessionEpoch = GetPreciseTime();
    }

    // Core gameplay
    InterlockedIncrement(&g_playerState->score);
    g_playerState->health = fmaxf(0.0f, g_playerState->health - 0.01f);

    // Position simulation (circular movement with gravity)
    static FLOAT angle = 0.0f;
    static bool isJumping = false;
    static ULONGLONG jumpStartTime = 0;
    const FLOAT radius = 50.0f;
    const FLOAT jumpHeight = 5.0f;
    angle += 0.01f;

    Vector3 oldPos = g_playerState->position;
    g_playerState->position.x = sinf(angle) * radius;
    g_playerState->position.z = cosf(angle) * radius;

    // Simulate jump physics (every 5 seconds)
    if (!isJumping && (angle > 3.0f || angle < -3.0f)) {
        if (static_cast<float>(rand()) / RAND_MAX < 0.05f) { // 5% chance to jump
            isJumping = true;
            jumpStartTime = GetPreciseTime();
        }
    }

    if (isJumping) {
        ULONGLONG elapsed = GetPreciseTime() - jumpStartTime;
        FLOAT t = static_cast<FLOAT>(elapsed) / g_qpcFreq; // Time in seconds

        // Parabolic jump trajectory
        g_playerState->position.y = 10.0f + jumpHeight * (4.0f * t - 4.0f * t * t);

        if (t >= 1.0f) {
            isJumping = false;
            g_playerState->position.y = 10.0f;
        }
    }
    else {
        g_playerState->position.y = 10.0f;
    }

    // Yaw simulation: smooth rotation with human limits
    static FLOAT yawVelocity = 0.0f;
    FLOAT maxDeltaYaw = MAX_YAW_PER_SECOND * 0.05f; // 50ms frame time

    // Random direction changes (simulating human aiming)
    if (static_cast<float>(rand()) / RAND_MAX < 0.01f) {
        yawVelocity = (static_cast<float>(rand()) / RAND_MAX * 2.0f - 1.0f) * MAX_YAW_PER_SECOND;
    }

    // Apply velocity with smoothing
    FLOAT desiredDelta = yawVelocity * 0.05f;
    FLOAT actualDelta = fmaxf(-maxDeltaYaw, fminf(maxDeltaYaw, desiredDelta));
    g_playerState->yaw += actualDelta;
    NormalizeYaw(g_playerState->yaw);

    // Pitch simulation: mostly level with occasional jumps
    static FLOAT pitchBase = 0.0f;
    static ULONGLONG lastPitchJump = 0;

    // 95% of time: near horizontal (-5° to 5°)
    // 5% of time: ±30° jumps (simulating jump/crouch)
    if (GetPreciseTime() - lastPitchJump > g_qpcFreq * 5.0f && (rand() % 100) < 5) {
        pitchBase = (rand() % 2 == 0) ? JUMP_PITCH_OFFSET : -JUMP_PITCH_OFFSET;
        lastPitchJump = GetPreciseTime();
    }
    else if (GetPreciseTime() - lastPitchJump > g_qpcFreq * 0.5f) {
        pitchBase = (static_cast<float>(rand()) / RAND_MAX * 10.0f) - 5.0f; // -5° to 5°
    }

    // Add micro-tremor for realism
    FLOAT tremor = (static_cast<float>(rand()) / RAND_MAX * 0.5f) - 0.25f;
    g_playerState->pitch = pitchBase + tremor;
    NormalizePitch(g_playerState->pitch);

    // Ammo simulation with realistic reload
    static DWORD lastShotTime = 0;
    static bool isReloading = false;
    static ULONGLONG reloadStartTime = 0;

    ULONGLONG now = GetPreciseTime();

    // Handle reload completion
    if (isReloading && (now - reloadStartTime) >= (RELOAD_DURATION_MS * g_qpcFreq / 1000)) {
        DWORD reloadAmount = min(30U - g_playerState->currentAmmo, g_playerState->reserveAmmo);
        g_playerState->currentAmmo += reloadAmount;
        g_playerState->reserveAmmo -= reloadAmount;
        isReloading = false;
    }

    // Automatic shooting (every 0.5s)
    if (!isReloading && (now - lastShotTime) > g_qpcFreq * 0.5f) {
        if (g_playerState->currentAmmo > 0) {
            g_playerState->currentAmmo--;
            lastShotTime = now;
        }
        else if (g_playerState->reserveAmmo > 0 && !isReloading) {
            // Start reload animation
            isReloading = true;
            reloadStartTime = now;
        }
    }

    // Integrity timestamps
    if (CalculatePositionDistance(oldPos, g_playerState->position) > 0.1f) {
        g_playerState->lastPositionUpdate = now;
    }
    g_playerState->lastUpdate = now;

    // Canary protection
    g_playerState->canary = static_cast<BYTE>(GetTickCount64() & 0xFF);

    // Recalculate checksum
    CalculateChecksum(g_playerState);

    ReleaseMutex(g_gameMutex);
}

void RenderGameScreen() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);

    COORD topLeft = { 0, 0 };
    DWORD written;
    FillConsoleOutputCharacterA(
        GetStdHandle(STD_OUTPUT_HANDLE),
        ' ',
        csbi.dwSize.X * csbi.dwSize.Y,
        topLeft,
        &written
    );
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), topLeft);

    std::cout << "=== VMX-SENTINEL SANDBOX v2.4 (PID: " << GetCurrentProcessId() << ") ===\n\n";
    std::cout << "Player State Address: 0x" << std::hex << reinterpret_cast<uintptr_t>(g_playerState) << std::dec << "\n";
    std::cout << "Session ID: " << g_playerState->sessionId << "\n";
    std::cout << "Score: " << g_playerState->score << " (0x" << std::hex << reinterpret_cast<uintptr_t>(&g_playerState->score) << ")\n";
    std::cout << "Health: " << std::fixed << std::setprecision(2) << g_playerState->health
        << " (0x" << std::hex << reinterpret_cast<uintptr_t>(&g_playerState->health) << ")\n";

    std::cout << "\n[POSITION & ORIENTATION]\n";
    std::cout << "Position (XYZ): ("
        << std::fixed << std::setprecision(2)
        << g_playerState->position.x << ", "
        << g_playerState->position.y << ", "
        << g_playerState->position.z << ") "
        << "(0x" << std::hex << reinterpret_cast<uintptr_t>(&g_playerState->position) << ")\n";
    std::cout << "Rotation: Pitch=" << std::fixed << std::setprecision(2) << g_playerState->pitch
        << " deg, Yaw=" << g_playerState->yaw << " deg "
        << "(0x" << std::hex << reinterpret_cast<uintptr_t>(&g_playerState->pitch)
        << "/0x" << reinterpret_cast<uintptr_t>(&g_playerState->yaw) << ")\n";

    std::cout << "\n[COMBAT STATUS]\n";
    std::cout << "Ammo: " << std::dec << g_playerState->currentAmmo << "/" << g_playerState->reserveAmmo
        << " (Current/Reserve) "
        << "(0x" << std::hex << reinterpret_cast<uintptr_t>(&g_playerState->currentAmmo)
        << "/0x" << reinterpret_cast<uintptr_t>(&g_playerState->reserveAmmo) << ")\n";

    std::cout << "\n[INTEGRITY PROTECTORS]\n";
    std::cout << "Canary Value: 0x" << std::hex << static_cast<int>(g_playerState->canary) << std::dec << "\n";
    std::cout << "Memory Checksum: 0x" << std::hex << static_cast<int>(g_playerState->checksum) << std::dec << "\n";
    std::cout << "Position Update: " << (GetPreciseTime() - g_playerState->lastPositionUpdate) / static_cast<double>(g_qpcFreq) << "s ago\n";

    std::cout << "\n[ATTACK SURFACE MAP]\n";
    std::cout << "High Value Targets:\n";
    std::cout << "  • Score (4 bytes)      @ offset 4\n";
    std::cout << "  • Health (4 bytes)     @ offset 8\n";
    std::cout << "  • Position (12 bytes)  @ offset 28\n";
    std::cout << "  • Rotation (8 bytes)   @ offset 40 (Pitch/Yaw)\n";
    std::cout << "  • Ammo (8 bytes)       @ offset 48\n";
    std::cout << "[!] Red team: All addresses are volatile - session rotation active\n";
    std::cout << "[!] Blue team: Monitor pitch/yaw acceleration for aimbots\n";
    std::cout << "[!] Ammo reloads take 300ms - detect instant reloads!\n";
    std::cout << "[!] Press ESC to exit...\n";
}

// ======================
// SEH-COMPATIBLE DEBUG TRAP MONITOR
// ======================
void DebugTrapMonitor() {
    // Use SafeReadByte function for SEH-safe memory access
    static BYTE trapValues[DEBUG_TRIP_COUNT] = { 0 };

    for (size_t i = 0; i < DEBUG_TRIP_COUNT; ++i) {
        BYTE currentValue;
        if (SafeReadByte(reinterpret_cast<const void*>(DEBUG_TRIP_ADDRESSES[i]), currentValue)) {
            if (trapValues[i] == 0) {
                trapValues[i] = currentValue;
            }
            else if (trapValues[i] != currentValue) {
                std::cerr << "\n[ALERT] Tripwire @ 0x" << std::hex << DEBUG_TRIP_ADDRESSES[i]
                    << ": 0x" << static_cast<int>(trapValues[i])
                    << " -> 0x" << static_cast<int>(currentValue) << "\n";
                trapValues[i] = currentValue;
            }
        }
        // If SafeReadByte fails, silently ignore (memory not readable)
    }

    // Canary integrity check
    static BYTE lastCanary = 0;
    if (g_playerState) {
        if (lastCanary != 0 && g_playerState->canary != lastCanary) {
            std::cerr << "\n[SECURITY BREACH] Canary value altered! (0x"
                << std::hex << static_cast<int>(lastCanary)
                << " -> 0x" << static_cast<int>(g_playerState->canary) << ")\n";
        }
        lastCanary = g_playerState->canary;

        // Position velocity analysis (max 10 m/s)
        static Vector3 lastPos = { 0 };
        static ULONGLONG lastPosTime = GetPreciseTime();
        ULONGLONG now = GetPreciseTime();
        FLOAT deltaTime = static_cast<FLOAT>(now - lastPosTime) / g_qpcFreq;

        if (deltaTime > 0.0f) {
            FLOAT distance = CalculatePositionDistance(lastPos, g_playerState->position);
            FLOAT velocity = distance / deltaTime;

            if (velocity > 10.0f) { // 10 m/s max human speed
                std::cerr << "\n[MOVEMENT ANOMALY] Velocity: "
                    << std::fixed << std::setprecision(2) << velocity << " m/s (threshold: 10.0)\n";
            }
        }

        lastPos = g_playerState->position;
        lastPosTime = now;

        // Pitch/Yaw acceleration analysis
        static FLOAT lastPitch = 0.0f;
        static FLOAT lastYaw = 0.0f;
        static ULONGLONG lastRotTime = GetPreciseTime();

        FLOAT deltaPitch = fabsf(g_playerState->pitch - lastPitch);
        FLOAT deltaYaw = fabsf(g_playerState->yaw - lastYaw);
        FLOAT rotDeltaTime = static_cast<FLOAT>(now - lastRotTime) / g_qpcFreq;

        if (rotDeltaTime > 0.001f) {
            FLOAT pitchAccel = deltaPitch / (rotDeltaTime * rotDeltaTime);
            FLOAT yawAccel = deltaYaw / (rotDeltaTime * rotDeltaTime);

            // Pitch: max 300 deg/s² (human limit)
            if (pitchAccel > MAX_PITCH_ACCEL) {
                std::cerr << "\n[AIMBOT DETECTED] Pitch acceleration: "
                    << std::fixed << std::setprecision(1) << pitchAccel
                    << " deg/s² (threshold: " << MAX_PITCH_ACCEL << ")\n";
            }

            // Yaw: max 120 deg/s (human wrist limit)
            FLOAT yawVel = deltaYaw / rotDeltaTime;
            if (yawVel > MAX_YAW_PER_SECOND) {
                std::cerr << "\n[AIMBOT DETECTED] Yaw velocity: "
                    << std::fixed << std::setprecision(1) << yawVel
                    << " deg/s (threshold: " << MAX_YAW_PER_SECOND << ")\n";
            }
        }

        lastPitch = g_playerState->pitch;
        lastYaw = g_playerState->yaw;
        lastRotTime = now;
    }
}

void GameMainThread() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpcFreq = freq.QuadPart;

    // Seed RNG for physics
    g_rng.seed(static_cast<unsigned int>(GetTickCount64()));

    g_playerState = reinterpret_cast<PlayerState*>(VirtualAlloc(
        NULL,
        4096,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    ));

    if (!g_playerState) {
        std::cerr << "FATAL: Memory allocation failed (0x" << std::hex << GetLastError() << ")\n";
        return;
    }

    memset(g_playerState, 0, sizeof(PlayerState));

    // Initialize game state with FPS physics
    g_playerState->sessionId = GetTickCount64() & 0xFFFFFFFF;
    g_playerState->score = 0;
    g_playerState->health = 100.0f;
    strcpy_s(g_playerState->playerName, "QKV-Expert");
    g_playerState->position = { 0.0f, 10.0f, 0.0f };
    g_playerState->pitch = 0.0f;    // Level horizontal
    g_playerState->yaw = 0.0f;      // Facing north
    g_playerState->currentAmmo = 30;
    g_playerState->reserveAmmo = 60;
    g_playerState->lastUpdate = GetPreciseTime();
    g_playerState->lastPositionUpdate = GetPreciseTime();
    g_playerState->canary = static_cast<BYTE>(GetTickCount64() & 0xFF);
    CalculateChecksum(g_playerState);

    // Console setup
    SetConsoleTitleA("VMX-Sentinel v4.2 (FPS Physics Edition)");
    CONSOLE_FONT_INFOEX font = { sizeof(font) };
    font.dwFontSize.Y = 16;
    wcscpy_s(font.FaceName, L"Consolas");
    SetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &font);

    // Main loop
    while (!g_exitRequested) {
        UpdateGameState();
        RenderGameScreen();
        DebugTrapMonitor();

        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            g_exitRequested = true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    VirtualFree(g_playerState, 0, MEM_RELEASE);
}

// ======================
// ENTRY POINT
// ======================
int main() {
    ApplyProcessMitigations();

    g_gameMutex = CreateMutexA(NULL, FALSE, "VMX_Sentinel_Mutex_2077");
    if (!g_gameMutex) {
        std::cerr << "FATAL: Mutex creation failed (0x" << std::hex << GetLastError() << ")\n";
        return 1;
    }

    std::thread gameThread(GameMainThread);

    // Memory pressure simulation
    std::vector<void*> scratchMemory;
    for (int i = 0; i < 256; ++i) {
        void* mem = VirtualAlloc(NULL, 4096, MEM_COMMIT, PAGE_READWRITE);
        if (mem) scratchMemory.push_back(mem);
    }

    gameThread.join();

    for (auto mem : scratchMemory) {
        VirtualFree(mem, 0, MEM_RELEASE);
    }
    CloseHandle(g_gameMutex);

    std::cout << "\nVMX-Sentinel shutdown complete. Physics data preserved for analysis.\n";
    return 0;
}