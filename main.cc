// main.cpp - VMX-SENTINEL PURE GAME LOGIC v3.1
// QKV Expert. Strictly game mechanics only - no detection logic.
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
constexpr DWORD AMMO_REFILL_DELAY_MS = 10000;  // 10 seconds to refill after 0/0
constexpr DWORD HEALTH_DRAIN_INTERVAL_MS = 10000; // 10 seconds stable health
constexpr DWORD HEALTH_DRAIN_DURATION_MS = 1000;  // 1 second rapid drain
constexpr DWORD HEALTH_RECOVERY_DURATION_MS = 5000; // 5 seconds linear recovery

#pragma pack(push, 1)
struct Vector3 {
    FLOAT x;
    FLOAT y;
    FLOAT z;
};

// PRECISE 80-BYTE LAYOUT (FPS PHYSICS OPTIMIZED) - PRESERVED OFFSETS
struct PlayerState {
    // Core identity section [0-27]
    DWORD sessionId;          // 0-3   (DWORD)
    volatile LONG score;      // 4-7   (LONG)
    FLOAT health;             // 8-11  (FLOAT)
    CHAR playerName[16];      // 12-27 (16 bytes CHAR array)

    // Position & orientation section [28-47] (SIMD ALIGNED)
    Vector3 position;         // 28-39 (12 bytes: x=28-31, y=32-35, z=36-39)
    FLOAT pitch;              // 40-43 (-90.0 to 90.0 degrees)
    FLOAT yaw;                // 44-47 (-180.0 to 180.0 degrees)

    // Combat section [48-55]
    DWORD currentAmmo;        // 48-51 (0-30)
    DWORD reserveAmmo;        // 52-55 (0-200)

    // Timing section [56-79] (maintains 80-byte layout)
    ULONGLONG lastUpdate;     // 56-63 (QPC timestamp)
    ULONGLONG lastAmmoRefill; // 64-71 (QPC timestamp for ammo refill)
    ULONGLONG lastHealthEvent; // 72-79 (QPC timestamp for health cycle)
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
// SECURITY ENHANCEMENTS (SYSTEM-LEVEL ONLY)
// ======================
void ApplyProcessMitigations() {
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) return;

    g_pSetProcessMitigationPolicy = (PSET_PROCESS_MITIGATION_POLICY)
        GetProcAddress(hKernel32, "SetProcessMitigationPolicy");

    if (!g_pSetProcessMitigationPolicy) {
        return; // Silent fail - not game-critical
    }

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicPolicy = { 0 };
    dynamicPolicy.ProhibitDynamicCode = TRUE;
    g_pSetProcessMitigationPolicy(ProcessDynamicCodePolicy, &dynamicPolicy, sizeof(dynamicPolicy));

    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sigPolicy = { 0 };
    sigPolicy.MicrosoftSignedOnly = TRUE;
    g_pSetProcessMitigationPolicy(ProcessSignaturePolicy, &sigPolicy, sizeof(sigPolicy));
}

// ======================
// CORE GAME LOGIC (FPS PHYSICS OPTIMIZED)
// ======================
ULONGLONG GetPreciseTime() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
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

void UpdateGameState() {
    WaitForSingleObject(g_gameMutex, INFINITE);

    // Session rotation (30-second cycle)
    static DWORD sessionEpoch = 0;
    if (GetPreciseTime() - sessionEpoch > g_qpcFreq * 30) {
        g_playerState->sessionId = GetTickCount64() & 0xFFFFFFFF;
        sessionEpoch = GetPreciseTime();
    }

    // Core gameplay: score always increments
    InterlockedIncrement(&g_playerState->score);

    // Health cycle: 10s stable -> 1s drain -> 5s recovery
    ULONGLONG now = GetPreciseTime();
    static bool isDraining = false;
    static bool isRecovering = false;
    static FLOAT recoveryStartHealth = 100.0f;

    DWORD timeSinceLastHealthEvent = static_cast<DWORD>((now - g_playerState->lastHealthEvent) * 1000 / g_qpcFreq);

    if (!isDraining && !isRecovering) {
        // Stable phase (10 seconds)
        if (!isDraining && timeSinceLastHealthEvent > HEALTH_DRAIN_INTERVAL_MS) {
            isDraining = true;
        }
    }

    if (isDraining) {
        // Rapid drain phase (1 second, drain 30% health)
        if (timeSinceLastHealthEvent > HEALTH_DRAIN_INTERVAL_MS + HEALTH_DRAIN_DURATION_MS) {
            g_playerState->health = fmaxf(0.0f, 70.0f); // Instant drain to 70%
            isDraining = false;
            isRecovering = true;
            recoveryStartHealth = 70.0f;
        }
    }

    if (isRecovering) {
        // Linear recovery phase (5 seconds back to 100%)
        DWORD recoveryTime = static_cast<DWORD>((now - g_playerState->lastHealthEvent) * 1000 / g_qpcFreq)
            - HEALTH_DRAIN_INTERVAL_MS - HEALTH_DRAIN_DURATION_MS;

        if (recoveryTime <= HEALTH_RECOVERY_DURATION_MS) {
            FLOAT recoveryProgress = static_cast<FLOAT>(recoveryTime) / HEALTH_RECOVERY_DURATION_MS;
            g_playerState->health = recoveryStartHealth + (100.0f - recoveryStartHealth) * recoveryProgress;
        }
        else {
            g_playerState->health = 100.0f;
            isRecovering = false;
            g_playerState->lastHealthEvent = now; // Reset cycle
        }
    }

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
            jumpStartTime = now;
        }
    }

    if (isJumping) {
        ULONGLONG elapsed = now - jumpStartTime;
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
    if (now - lastPitchJump > g_qpcFreq * 5.0f && (rand() % 100) < 5) {
        pitchBase = (rand() % 2 == 0) ? JUMP_PITCH_OFFSET : -JUMP_PITCH_OFFSET;
        lastPitchJump = now;
    }
    else if (now - lastPitchJump > g_qpcFreq * 0.5f) {
        pitchBase = (static_cast<float>(rand()) / RAND_MAX * 10.0f) - 5.0f; // -5° to 5°
    }

    // Add micro-tremor for realism
    FLOAT tremor = (static_cast<float>(rand()) / RAND_MAX * 0.5f) - 0.25f;
    g_playerState->pitch = pitchBase + tremor;
    NormalizePitch(g_playerState->pitch);

    // Ammo simulation with realistic reload AND full refill after depletion
    static DWORD lastShotTime = 0;
    static bool isReloading = false;
    static ULONGLONG reloadStartTime = 0;

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

    // Full ammo refill after complete depletion (0/0 state)
    if (g_playerState->currentAmmo == 0 && g_playerState->reserveAmmo == 0) {
        if (g_playerState->lastAmmoRefill == 0) {
            g_playerState->lastAmmoRefill = now; // Mark depletion start time
        }
        else if ((now - g_playerState->lastAmmoRefill) >= (AMMO_REFILL_DELAY_MS * g_qpcFreq / 1000)) {
            // Refill after 10 seconds
            g_playerState->currentAmmo = 30;
            g_playerState->reserveAmmo = 60;
            g_playerState->lastAmmoRefill = 0; // Reset refill timer
        }
    }
    else {
        g_playerState->lastAmmoRefill = 0; // Reset if ammo was replenished
    }

    // Update timestamps (game logic only)
    g_playerState->lastUpdate = now;

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

    // PID displayed in decimal as requested
    std::cout << "=== VMX-SENTINEL SANDBOX v3.1 (PID: " << GetCurrentProcessId() << ") ===\n\n";
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

    std::cout << "\n[MEMORY LAYOUT - CRITICAL FOR ANALYSIS]\n";
    std::cout << "* Session ID:     DWORD @ offset 0   (0x" << std::hex << offsetof(PlayerState, sessionId) << ")\n";
    std::cout << "* Score:          LONG  @ offset 4   (0x" << offsetof(PlayerState, score) << ")\n";
    std::cout << "* Health:         FLOAT @ offset 8   (0x" << offsetof(PlayerState, health) << ")\n";
    std::cout << "* PlayerName:     CHAR[16] @ offset 12 (0x" << offsetof(PlayerState, playerName) << ")\n";
    std::cout << "* Position.x:     FLOAT @ offset 28  (0x" << offsetof(PlayerState, position) << ")\n";
    std::cout << "* Position.y:     FLOAT @ offset 32\n";
    std::cout << "* Position.z:     FLOAT @ offset 36\n";
    std::cout << "* Pitch:          FLOAT @ offset 40  (0x" << reinterpret_cast<uintptr_t>(&g_playerState->pitch) - reinterpret_cast<uintptr_t>(g_playerState) << ")\n";
    std::cout << "* Yaw:            FLOAT @ offset 44  (0x" << reinterpret_cast<uintptr_t>(&g_playerState->yaw) - reinterpret_cast<uintptr_t>(g_playerState) << ")\n";
    std::cout << "* CurrentAmmo:    DWORD @ offset 48  (0x" << offsetof(PlayerState, currentAmmo) << ")\n";
    std::cout << "* ReserveAmmo:    DWORD @ offset 52  (0x" << offsetof(PlayerState, reserveAmmo) << ")\n";
    std::cout << "* LastUpdate:     ULONGLONG @ offset 56\n";

    std::cout << "\n[GAME MECHANICS]\n";
    std::cout << "* Session rotates every 30 seconds\n";
    std::cout << "* Health: 10s stable -> 1s drain (30%) -> 5s recovery\n";
    std::cout << "* Ammo: Full refill after 10 seconds at 0/0 state\n";
    std::cout << "* Realistic reload time: 300ms\n";
    std::cout << "* Human-limited aiming (120 deg/s max)\n";
    std::cout << "\nPress ESC to exit...\n";
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
    g_playerState->lastHealthEvent = GetPreciseTime(); // Start health cycle

    // Console setup
    SetConsoleTitleA("VMX-Sentinel v3.1 (Game Logic Only)");
    CONSOLE_FONT_INFOEX font = { sizeof(font) };
    font.dwFontSize.Y = 16;
    wcscpy_s(font.FaceName, L"Lucida Console");
    SetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &font);

    // Main loop
    while (!g_exitRequested) {
        UpdateGameState();
        RenderGameScreen();

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

    std::cout << "\nVMX-Sentinel shutdown complete.\n";
    return 0;
}