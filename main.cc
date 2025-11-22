// =====================================================
// VMX-SENTINEL: KERNEL VIRTUALIZATION ANTI-CHEAT ARENA
// Official Sandbox for Red Team (G老师) vs Blue Team (轻子)
// Expert Analysis by QKV - World's Leading Hypervisor Architect
// =====================================================
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

constexpr FLOAT MAX_YAW_PER_SECOND = 120.0f;
constexpr FLOAT MAX_PITCH_ACCEL = 300.0f;
constexpr FLOAT JUMP_PITCH_OFFSET = 30.0f;
constexpr DWORD RELOAD_DURATION_MS = 300;
constexpr DWORD AMMO_REFILL_DELAY_MS = 10000;
constexpr DWORD HEALTH_DRAIN_INTERVAL_MS = 10000;
constexpr DWORD HEALTH_DRAIN_DURATION_MS = 1000;
constexpr DWORD HEALTH_RECOVERY_DURATION_MS = 5000;
constexpr DWORD MIN_SHOOT_INTERVAL_MS = 100;
constexpr DWORD MAX_SHOOT_INTERVAL_MS = 800;
constexpr DWORD CLIP_SIZE = 30;
constexpr DWORD MAX_RESERVE_AMMO = 200;
constexpr SHORT CONSOLE_WIDTH = 120;
constexpr SHORT CONSOLE_HEIGHT = 40;

#pragma pack(push, 1)
struct Vector3 {
    FLOAT x;
    FLOAT y;
    FLOAT z;
};

struct PlayerState {
    DWORD sessionId;
    volatile LONG score;
    FLOAT health;
    CHAR playerName[16];
    Vector3 position;
    FLOAT pitch;
    FLOAT yaw;
    DWORD currentClip;
    DWORD reserveAmmo;
    ULONGLONG lastUpdate;
    ULONGLONG lastAmmoRefill;
    ULONGLONG lastHealthEvent;
};
static_assert(sizeof(PlayerState) == 80, "CRITICAL: PlayerState MUST be exactly 80 bytes");
#pragma pack(pop)

std::atomic<bool> g_exitRequested = false;
HANDLE g_gameMutex = nullptr;
PlayerState* g_playerState = nullptr;
ULONGLONG g_qpcFreq = 0;
PSET_PROCESS_MITIGATION_POLICY g_pSetProcessMitigationPolicy = nullptr;
std::mt19937 g_rng;
CHAR_INFO* g_consoleBuffer = nullptr;

void InitializeConsole() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    GetConsoleMode(hOut, &mode);
    mode |= ENABLE_QUICK_EDIT_MODE | ENABLE_EXTENDED_FLAGS;
    SetConsoleMode(hOut, mode);

    COORD bufferSize = { CONSOLE_WIDTH, CONSOLE_HEIGHT };
    SetConsoleScreenBufferSize(hOut, bufferSize);

    SMALL_RECT windowRect = { 0, 0, CONSOLE_WIDTH - 1, CONSOLE_HEIGHT - 1 };
    SetConsoleWindowInfo(hOut, TRUE, &windowRect);

    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hOut, &cursorInfo);
    cursorInfo.bVisible = FALSE;
    cursorInfo.dwSize = 1;
    SetConsoleCursorInfo(hOut, &cursorInfo);

    SetConsoleTitleA("VMX-Sentinel | Kernel Virtualization Arena 2077");

    g_consoleBuffer = new CHAR_INFO[CONSOLE_WIDTH * CONSOLE_HEIGHT];
    memset(g_consoleBuffer, 0, sizeof(CHAR_INFO) * CONSOLE_WIDTH * CONSOLE_HEIGHT);
}

void CleanupConsole() {
    if (g_consoleBuffer) {
        delete[] g_consoleBuffer;
        g_consoleBuffer = nullptr;
    }

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hOut, &cursorInfo);
    cursorInfo.bVisible = TRUE;
    cursorInfo.dwSize = 20;
    SetConsoleCursorInfo(hOut, &cursorInfo);
}

void RenderToBuffer(const std::string& content) {
    if (!g_consoleBuffer) return;

    for (int i = 0; i < CONSOLE_WIDTH * CONSOLE_HEIGHT; i++) {
        g_consoleBuffer[i].Char.AsciiChar = ' ';
        g_consoleBuffer[i].Attributes = 7;
    }

    std::istringstream iss(content);
    std::string line;
    int y = 0;

    while (std::getline(iss, line) && y < CONSOLE_HEIGHT) {
        if (line.length() > CONSOLE_WIDTH) {
            line = line.substr(0, CONSOLE_WIDTH);
        }

        for (int x = 0; x < (int)line.length() && x < CONSOLE_WIDTH; x++) {
            g_consoleBuffer[y * CONSOLE_WIDTH + x].Char.AsciiChar = line[x];
            g_consoleBuffer[y * CONSOLE_WIDTH + x].Attributes = 7;
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

    WriteConsoleOutputA(hOut, g_consoleBuffer, bufferSize, bufferCoord, &writeRegion);
}

inline bool SafeReadByte(const volatile void* address, BYTE& value) {
    __try {
        value = *(reinterpret_cast<const volatile BYTE*>(address));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ApplyProcessMitigations() {
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) return;

    g_pSetProcessMitigationPolicy = (PSET_PROCESS_MITIGATION_POLICY)
        GetProcAddress(hKernel32, "SetProcessMitigationPolicy");

    if (!g_pSetProcessMitigationPolicy) {
        return;
    }

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicPolicy = { 0 };
    dynamicPolicy.ProhibitDynamicCode = TRUE;
    g_pSetProcessMitigationPolicy(ProcessDynamicCodePolicy, &dynamicPolicy, sizeof(dynamicPolicy));

    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sigPolicy = { 0 };
    sigPolicy.MicrosoftSignedOnly = TRUE;
    g_pSetProcessMitigationPolicy(ProcessSignaturePolicy, &sigPolicy, sizeof(sigPolicy));
}

ULONGLONG GetPreciseTime() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

void NormalizeYaw(FLOAT& yaw) {
    yaw = fmodf(yaw, 360.0f);
    if (yaw > 180.0f) yaw -= 360.0f;
    if (yaw < -180.0f) yaw += 360.0f;
}

void NormalizePitch(FLOAT& pitch) {
    pitch = fmaxf(-90.0f, fminf(90.0f, pitch));
}

void UpdateGameState() {
    WaitForSingleObject(g_gameMutex, INFINITE);

    static DWORD sessionEpoch = 0;
    ULONGLONG now = GetPreciseTime();
    if (now - sessionEpoch > g_qpcFreq * 30) {
        g_playerState->sessionId = GetTickCount64() & 0xFFFFFFFF;
        sessionEpoch = now;
    }

    InterlockedIncrement(&g_playerState->score);

    static bool isDraining = false;
    static bool isRecovering = false;
    static FLOAT recoveryStartHealth = 100.0f;

    DWORD timeSinceLastHealthEvent = static_cast<DWORD>((now - g_playerState->lastHealthEvent) * 1000 / g_qpcFreq);

    if (!isDraining && !isRecovering) {
        if (timeSinceLastHealthEvent > HEALTH_DRAIN_INTERVAL_MS) {
            isDraining = true;
        }
    }

    if (isDraining) {
        if (timeSinceLastHealthEvent > HEALTH_DRAIN_INTERVAL_MS + HEALTH_DRAIN_DURATION_MS) {
            g_playerState->health = fmaxf(0.0f, 70.0f);
            isDraining = false;
            isRecovering = true;
            recoveryStartHealth = 70.0f;
        }
    }

    if (isRecovering) {
        DWORD recoveryTime = static_cast<DWORD>((now - g_playerState->lastHealthEvent) * 1000 / g_qpcFreq)
            - HEALTH_DRAIN_INTERVAL_MS - HEALTH_DRAIN_DURATION_MS;

        if (recoveryTime <= HEALTH_RECOVERY_DURATION_MS) {
            FLOAT recoveryProgress = static_cast<FLOAT>(recoveryTime) / HEALTH_RECOVERY_DURATION_MS;
            g_playerState->health = recoveryStartHealth + (100.0f - recoveryStartHealth) * recoveryProgress;
        }
        else {
            g_playerState->health = 100.0f;
            isRecovering = false;
            g_playerState->lastHealthEvent = now;
        }
    }

    static FLOAT angle = 0.0f;
    static bool isJumping = false;
    static ULONGLONG jumpStartTime = 0;
    const FLOAT radius = 50.0f;
    const FLOAT jumpHeight = 5.0f;
    angle += 0.01f;

    g_playerState->position.x = sinf(angle) * radius;
    g_playerState->position.z = cosf(angle) * radius;

    if (!isJumping && (angle > 3.0f || angle < -3.0f)) {
        if (static_cast<float>(rand()) / RAND_MAX < 0.05f) {
            isJumping = true;
            jumpStartTime = now;
        }
    }

    if (isJumping) {
        ULONGLONG elapsed = now - jumpStartTime;
        FLOAT t = static_cast<FLOAT>(elapsed) / g_qpcFreq;

        g_playerState->position.y = 10.0f + jumpHeight * (4.0f * t - 4.0f * t * t);

        if (t >= 1.0f) {
            isJumping = false;
            g_playerState->position.y = 10.0f;
        }
    }
    else {
        g_playerState->position.y = 10.0f;
    }

    static FLOAT yawVelocity = 0.0f;
    FLOAT maxDeltaYaw = MAX_YAW_PER_SECOND * 0.05f;

    if (static_cast<float>(rand()) / RAND_MAX < 0.01f) {
        yawVelocity = (static_cast<float>(rand()) / RAND_MAX * 2.0f - 1.0f) * MAX_YAW_PER_SECOND;
    }

    FLOAT desiredDelta = yawVelocity * 0.05f;
    FLOAT actualDelta = fmaxf(-maxDeltaYaw, fminf(maxDeltaYaw, desiredDelta));
    g_playerState->yaw += actualDelta;
    NormalizeYaw(g_playerState->yaw);

    static FLOAT pitchBase = 0.0f;
    static ULONGLONG lastPitchJump = 0;

    if (now - lastPitchJump > g_qpcFreq * 5.0f && (rand() % 100) < 5) {
        pitchBase = (rand() % 2 == 0) ? JUMP_PITCH_OFFSET : -JUMP_PITCH_OFFSET;
        lastPitchJump = now;
    }
    else if (now - lastPitchJump > g_qpcFreq * 0.5f) {
        pitchBase = (static_cast<float>(rand()) / RAND_MAX * 10.0f) - 5.0f;
    }

    FLOAT tremor = (static_cast<float>(rand()) / RAND_MAX * 0.5f) - 0.25f;
    g_playerState->pitch = pitchBase + tremor;
    NormalizePitch(g_playerState->pitch);

    static DWORD lastShotTime = 0;
    static bool isReloading = false;
    static ULONGLONG reloadStartTime = 0;
    static DWORD nextShotDelay = MIN_SHOOT_INTERVAL_MS + (rand() % (MAX_SHOOT_INTERVAL_MS - MIN_SHOOT_INTERVAL_MS));

    if (isReloading && (now - reloadStartTime) >= (RELOAD_DURATION_MS * g_qpcFreq / 1000)) {
        DWORD spaceInClip = CLIP_SIZE - g_playerState->currentClip;
        DWORD bulletsToLoad = min(spaceInClip, g_playerState->reserveAmmo);

        g_playerState->currentClip += bulletsToLoad;
        g_playerState->reserveAmmo -= bulletsToLoad;
        isReloading = false;
    }

    if (!isReloading) {
        DWORD timeSinceLastShot = static_cast<DWORD>((now - lastShotTime) * 1000 / g_qpcFreq);

        if (timeSinceLastShot > nextShotDelay && g_playerState->currentClip > 0) {
            g_playerState->currentClip--;
            lastShotTime = now;
            InterlockedIncrement(&g_playerState->score);
            nextShotDelay = MIN_SHOOT_INTERVAL_MS + (rand() % (MAX_SHOOT_INTERVAL_MS - MIN_SHOOT_INTERVAL_MS));
        }

        if (g_playerState->currentClip == 0 && g_playerState->reserveAmmo > 0) {
            isReloading = true;
            reloadStartTime = now;
        }
        else if (g_playerState->currentClip < 5) {
            if ((rand() % 100) < 75) {
                isReloading = true;
                reloadStartTime = now;
            }
        }
        else if ((rand() % 100) < 5) {
            isReloading = true;
            reloadStartTime = now;
        }
    }

    if (g_playerState->currentClip == 0 && g_playerState->reserveAmmo == 0) {
        if (g_playerState->lastAmmoRefill == 0) {
            g_playerState->lastAmmoRefill = now;
        }
        else if ((now - g_playerState->lastAmmoRefill) >= (AMMO_REFILL_DELAY_MS * g_qpcFreq / 1000)) {
            g_playerState->currentClip = CLIP_SIZE;
            g_playerState->reserveAmmo = MAX_RESERVE_AMMO;
            g_playerState->lastAmmoRefill = 0;
        }
    }
    else {
        g_playerState->lastAmmoRefill = 0;
    }

    g_playerState->lastUpdate = now;

    ReleaseMutex(g_gameMutex);
}

void RenderGameScreen() {
    std::ostringstream oss;

    oss << "=== [VMX-SENTINEL] FPS SANDBOX - KERNEL VIRTUALIZATION ARENA 2077 ===\n";
    oss << "==========================================================================\n\n";

    oss << "Player State Address: 0x" << std::hex << reinterpret_cast<uintptr_t>(g_playerState) << std::dec << "\n";
    oss << "Session ID: " << g_playerState->sessionId << " (PID: " << GetCurrentProcessId() << ")\n";
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

    oss << "\n[FPS PHYSICS PARAMETERS]\n";
    oss << "* Human yaw limit: 120 deg/s | Pitch accel: 300 deg/s^2\n";
    oss << "* Jump pitch offset: ±30 deg | Reload time: 300ms\n";
    oss << "* Health cycle: 10s stable → 1s drain (30%) → 5s recovery\n";
    oss << "* Ammo refill: 10s after complete depletion\n";

    RenderToBuffer(oss.str());
    SwapBuffers();
}

void GameMainThread() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpcFreq = freq.QuadPart;

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

    g_playerState->sessionId = GetTickCount64() & 0xFFFFFFFF;
    g_playerState->score = 0;
    g_playerState->health = 100.0f;
    strcpy_s(g_playerState->playerName, "QKV-Expert");
    g_playerState->position = { 0.0f, 10.0f, 0.0f };
    g_playerState->pitch = 0.0f;
    g_playerState->yaw = 0.0f;
    g_playerState->currentClip = CLIP_SIZE;
    g_playerState->reserveAmmo = MAX_RESERVE_AMMO;
    g_playerState->lastUpdate = GetPreciseTime();
    g_playerState->lastHealthEvent = GetPreciseTime();

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

int main() {
    InitializeConsole();

    ApplyProcessMitigations();

    g_gameMutex = CreateMutexA(NULL, FALSE, "VMX_Sentinel_Mutex_2077");
    if (!g_gameMutex) {
        std::cerr << "FATAL: Mutex creation failed (0x" << std::hex << GetLastError() << ")\n";
        CleanupConsole();
        return 1;
    }

    std::thread gameThread(GameMainThread);

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

    CleanupConsole();

    std::cout << "\nVMX-Sentinel shutdown complete.\n";
    return 0;
}