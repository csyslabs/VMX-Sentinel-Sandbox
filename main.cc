// main.cpp - VMX-SENTINEL PURE GAME LOGIC v3.4
// QKV Expert. Zero flicker console with copy support.
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
#include <sstream>

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
constexpr DWORD AMMO_REFILL_DELAY_MS = 10000;  // 10 seconds to refill after complete depletion
constexpr DWORD HEALTH_DRAIN_INTERVAL_MS = 10000; // 10 seconds stable health
constexpr DWORD HEALTH_DRAIN_DURATION_MS = 1000;  // 1 second rapid drain
constexpr DWORD HEALTH_RECOVERY_DURATION_MS = 5000; // 5 seconds linear recovery
constexpr DWORD MIN_SHOOT_INTERVAL_MS = 100;   // Minimum time between shots (100ms)
constexpr DWORD MAX_SHOOT_INTERVAL_MS = 800;   // Maximum time between shots (800ms)
constexpr DWORD CLIP_SIZE = 30;                // Standard magazine capacity
constexpr DWORD MAX_RESERVE_AMMO = 200;        // Maximum reserve ammo capacity
constexpr SHORT CONSOLE_WIDTH = 100;           // Fixed console width
constexpr SHORT CONSOLE_HEIGHT = 35;           // Fixed console height

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

    // Combat section [48-55] - CORRECTED SEMANTICS
    DWORD currentClip;        // 48-51 (Current magazine remaining, 0-30)
    DWORD reserveAmmo;        // 52-55 (Total reserve ammo in inventory, EXCLUDING current clip)

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
CHAR_INFO* g_consoleBuffer = nullptr; // Double-buffering for flicker-free rendering

// ======================
// CONSOLE DISPLAY OPTIMIZATION (ZERO FLICKER)
// ======================
void InitializeConsole() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    // Set console mode to allow selection/copy
    DWORD mode;
    GetConsoleMode(hOut, &mode);
    mode |= ENABLE_QUICK_EDIT_MODE | ENABLE_EXTENDED_FLAGS;
    SetConsoleMode(hOut, mode);

    // Set exact buffer size = window size (no scrollbars)
    COORD bufferSize = { CONSOLE_WIDTH, CONSOLE_HEIGHT };
    SetConsoleScreenBufferSize(hOut, bufferSize);

    // Set exact window size
    SMALL_RECT windowRect = { 0, 0, CONSOLE_WIDTH - 1, CONSOLE_HEIGHT - 1 };
    SetConsoleWindowInfo(hOut, TRUE, &windowRect);

    // Hide cursor permanently
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hOut, &cursorInfo);
    cursorInfo.bVisible = FALSE;
    cursorInfo.dwSize = 1;
    SetConsoleCursorInfo(hOut, &cursorInfo);

    // Set console title
    SetConsoleTitleA("VMX-Sentinel v3.4 (FPS Combat Logic)");

    // Allocate double buffer
    g_consoleBuffer = new CHAR_INFO[CONSOLE_WIDTH * CONSOLE_HEIGHT];
    memset(g_consoleBuffer, 0, sizeof(CHAR_INFO) * CONSOLE_WIDTH * CONSOLE_HEIGHT);
}

void CleanupConsole() {
    if (g_consoleBuffer) {
        delete[] g_consoleBuffer;
        g_consoleBuffer = nullptr;
    }

    // Restore cursor on exit
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hOut, &cursorInfo);
    cursorInfo.bVisible = TRUE;
    cursorInfo.dwSize = 20;
    SetConsoleCursorInfo(hOut, &cursorInfo);
}

void RenderToBuffer(const std::string& content) {
    if (!g_consoleBuffer) return;

    // Clear buffer
    for (int i = 0; i < CONSOLE_WIDTH * CONSOLE_HEIGHT; i++) {
        g_consoleBuffer[i].Char.AsciiChar = ' ';
        g_consoleBuffer[i].Attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }

    // Parse content into buffer lines
    std::istringstream iss(content);
    std::string line;
    int y = 0;

    while (std::getline(iss, line) && y < CONSOLE_HEIGHT) {
        // Truncate line if too long
        if (line.length() > CONSOLE_WIDTH) {
            line = line.substr(0, CONSOLE_WIDTH);
        }

        // Copy characters to buffer
        for (int x = 0; x < (int)line.length() && x < CONSOLE_WIDTH; x++) {
            g_consoleBuffer[y * CONSOLE_WIDTH + x].Char.AsciiChar = line[x];
            g_consoleBuffer[y * CONSOLE_WIDTH + x].Attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        }
        y++;
    }
}

void SwapBuffers() {
    if (!g_consoleBuffer) return;

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD bufferSize = { CONSOLE_WIDTH, CONSOLE_HEIGHT };
    COORD bufferCoord = { 0, 0 };
    SMALL_RECT writeRegion = { 0, 0, CONSOLE_WIDTH - 1, CONSOLE_HEIGHT - 1 };

    WriteConsoleOutputA(
        hOut,
        g_consoleBuffer,
        bufferSize,
        bufferCoord,
        &writeRegion
    );
}

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
    ULONGLONG now = GetPreciseTime();
    if (now - sessionEpoch > g_qpcFreq * 30) {
        g_playerState->sessionId = GetTickCount64() & 0xFFFFFFFF;
        sessionEpoch = now;
    }

    // Core gameplay: score always increments
    InterlockedIncrement(&g_playerState->score);

    // Health cycle: 10s stable -> 1s drain -> 5s recovery
    static bool isDraining = false;
    static bool isRecovering = false;
    static FLOAT recoveryStartHealth = 100.0f;

    DWORD timeSinceLastHealthEvent = static_cast<DWORD>((now - g_playerState->lastHealthEvent) * 1000 / g_qpcFreq);

    if (!isDraining && !isRecovering) {
        // Stable phase (10 seconds)
        if (timeSinceLastHealthEvent > HEALTH_DRAIN_INTERVAL_MS) {
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

    // ======================
    // REALISTIC AMMO SYSTEM (FPS STANDARD)
    // ======================
    static DWORD lastShotTime = 0;
    static bool isReloading = false;
    static ULONGLONG reloadStartTime = 0;
    static DWORD nextShotDelay = MIN_SHOOT_INTERVAL_MS + (rand() % (MAX_SHOOT_INTERVAL_MS - MIN_SHOOT_INTERVAL_MS));

    // Handle reload completion
    if (isReloading && (now - reloadStartTime) >= (RELOAD_DURATION_MS * g_qpcFreq / 1000)) {
        // Calculate how many bullets we can add to the clip
        DWORD spaceInClip = CLIP_SIZE - g_playerState->currentClip;
        DWORD bulletsToLoad = min(spaceInClip, g_playerState->reserveAmmo);

        // Load bullets into clip
        g_playerState->currentClip += bulletsToLoad;
        g_playerState->reserveAmmo -= bulletsToLoad;
        isReloading = false;
    }

    // Shooting logic with realistic intervals
    if (!isReloading) {
        DWORD timeSinceLastShot = static_cast<DWORD>((now - lastShotTime) * 1000 / g_qpcFreq);

        // Random shooting behavior (simulates human trigger finger)
        if (timeSinceLastShot > nextShotDelay && g_playerState->currentClip > 0) {
            // Fire one bullet
            g_playerState->currentClip--;
            lastShotTime = now;

            // Update score for each shot
            InterlockedIncrement(&g_playerState->score);

            // Set next random shooting interval
            nextShotDelay = MIN_SHOOT_INTERVAL_MS + (rand() % (MAX_SHOOT_INTERVAL_MS - MIN_SHOOT_INTERVAL_MS));
        }

        // Intelligent reload decisions
        if (g_playerState->currentClip == 0 && g_playerState->reserveAmmo > 0) {
            // Empty clip - must reload
            isReloading = true;
            reloadStartTime = now;
        }
        else if (g_playerState->currentClip < 5) {
            // Low ammo - 75% chance to reload
            if ((rand() % 100) < 75) {
                isReloading = true;
                reloadStartTime = now;
            }
        }
        else if ((rand() % 100) < 5) {
            // Random tactical reload (5% chance when clip is healthy)
            isReloading = true;
            reloadStartTime = now;
        }
    }

    // Full ammo refill after complete depletion (0 in clip + 0 reserve)
    if (g_playerState->currentClip == 0 && g_playerState->reserveAmmo == 0) {
        if (g_playerState->lastAmmoRefill == 0) {
            g_playerState->lastAmmoRefill = now; // Mark depletion start time
        }
        else if ((now - g_playerState->lastAmmoRefill) >= (AMMO_REFILL_DELAY_MS * g_qpcFreq / 1000)) {
            // Refill after 10 seconds (standard respawn)
            g_playerState->currentClip = CLIP_SIZE;
            g_playerState->reserveAmmo = MAX_RESERVE_AMMO;
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
    std::ostringstream oss;

    // PID displayed in decimal as requested
    oss << "=== VMX-SENTINEL SANDBOX v3.4 (PID: " << GetCurrentProcessId() << ") ===\n\n";
    oss << "Player State Address: 0x" << std::hex << reinterpret_cast<uintptr_t>(g_playerState) << std::dec << "\n";
    oss << "Session ID: " << g_playerState->sessionId << "\n";
    oss << "Score: " << g_playerState->score << " (0x" << std::hex << reinterpret_cast<uintptr_t>(&g_playerState->score) << ")\n";
    oss << "Health: " << std::fixed << std::setprecision(2) << g_playerState->health
        << " (0x" << std::hex << reinterpret_cast<uintptr_t>(&g_playerState->health) << ")\n";

    oss << "\n[POSITION & ORIENTATION]\n";
    oss << "Position (XYZ): ("
        << std::fixed << std::setprecision(2)
        << g_playerState->position.x << ", "
        << g_playerState->position.y << ", "
        << g_playerState->position.z << ") "
        << "(0x" << std::hex << reinterpret_cast<uintptr_t>(&g_playerState->position) << ")\n";
    oss << "Rotation: Pitch=" << std::fixed << std::setprecision(2) << g_playerState->pitch
        << " deg, Yaw=" << g_playerState->yaw << " deg "
        << "(0x" << std::hex << reinterpret_cast<uintptr_t>(&g_playerState->pitch)
        << "/0x" << reinterpret_cast<uintptr_t>(&g_playerState->yaw) << ")\n";

    oss << "\n[COMBAT STATUS]\n";
    // CORRECTED SINGLE-LINE AMMO DISPLAY (100% DECIMAL)
    oss << "Ammo: " << std::dec << g_playerState->currentClip << "/" << g_playerState->reserveAmmo
        << " (Clip/Reserve) | Capacity:" << CLIP_SIZE
        << " | Total:" << (g_playerState->currentClip + g_playerState->reserveAmmo) << "\n";
    oss << "      Memory: (0x" << std::hex << reinterpret_cast<uintptr_t>(&g_playerState->currentClip)
        << "/0x" << reinterpret_cast<uintptr_t>(&g_playerState->reserveAmmo) << ")\n";

    oss << "\n[MEMORY LAYOUT - CRITICAL FOR ANALYSIS]\n";
    oss << "* Session ID:     DWORD @ offset 0   (0x" << std::hex << offsetof(PlayerState, sessionId) << ")\n";
    oss << "* Score:          LONG  @ offset 4   (0x" << offsetof(PlayerState, score) << ")\n";
    oss << "* Health:         FLOAT @ offset 8   (0x" << offsetof(PlayerState, health) << ")\n";
    oss << "* PlayerName:     CHAR[16] @ offset 12 (0x" << offsetof(PlayerState, playerName) << ")\n";
    oss << "* Position.x:     FLOAT @ offset 28  (0x" << offsetof(PlayerState, position) << ")\n";
    oss << "* Position.y:     FLOAT @ offset 32\n";
    oss << "* Position.z:     FLOAT @ offset 36\n";
    oss << "* Pitch:          FLOAT @ offset 40  (0x" << reinterpret_cast<uintptr_t>(&g_playerState->pitch) - reinterpret_cast<uintptr_t>(g_playerState) << ")\n";
    oss << "* Yaw:            FLOAT @ offset 44  (0x" << reinterpret_cast<uintptr_t>(&g_playerState->yaw) - reinterpret_cast<uintptr_t>(g_playerState) << ")\n";
    oss << "* CurrentClip:    DWORD @ offset 48  (0x" << offsetof(PlayerState, currentClip) << ")\n";
    oss << "* ReserveAmmo:    DWORD @ offset 52  (0x" << offsetof(PlayerState, reserveAmmo) << ")\n";
    oss << "* LastUpdate:     ULONGLONG @ offset 56\n";

    oss << "\n[GAME MECHANICS]\n";
    oss << "* Session rotates every 30 seconds\n";
    oss << "* Health: 10s stable -> 1s drain (30%) -> 5s recovery\n";
    oss << "* Ammo: 30-round clip, random shooting intervals (100-800ms)\n";
    oss << "* Tactical reloads when clip < 5 bullets (75% chance)\n";
    oss << "* Full refill after 10 seconds at complete depletion\n";
    oss << "* Human-limited aiming (120 deg/s max)\n";
    oss << "\nPress ESC to exit...\n";

    // Render to double buffer
    RenderToBuffer(oss.str());

    // Swap buffers (atomic screen update - ZERO FLICKER)
    SwapBuffers();
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
    g_playerState->currentClip = CLIP_SIZE;  // Full clip
    g_playerState->reserveAmmo = MAX_RESERVE_AMMO; // Full reserve
    g_playerState->lastUpdate = GetPreciseTime();
    g_playerState->lastHealthEvent = GetPreciseTime(); // Start health cycle

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
    // CRITICAL: Initialize console FIRST
    InitializeConsole();

    ApplyProcessMitigations();

    g_gameMutex = CreateMutexA(NULL, FALSE, "VMX_Sentinel_Mutex_2077");
    if (!g_gameMutex) {
        std::cerr << "FATAL: Mutex creation failed (0x" << std::hex << GetLastError() << ")\n";
        CleanupConsole();
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

    // Cleanup console resources
    CleanupConsole();

    std::cout << "\nVMX-Sentinel shutdown complete.\n";
    return 0;
}