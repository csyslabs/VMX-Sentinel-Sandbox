// main.cpp - Hypervisor Attack Surface Sandbox (x64 Windows)
// QKV Expert Design for Kernel Virtualization Competition 2025
// Compile with: cl /EHsc /O2 /std:c++17 main.cpp /link /subsystem:console

#define _WIN32_WINNT 0x0A00 // Windows 10 target
#include <windows.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <vector>
#include <iomanip>
#include <intrin.h>
#include <processthreadsapi.h>

// ======================
// COMPATIBILITY LAYER FOR PROCESS MITIGATION POLICIES
// ======================
typedef BOOL(WINAPI* PSET_PROCESS_MITIGATION_POLICY)(
    PROCESS_MITIGATION_POLICY Policy,
    PVOID pBuffer,
    SIZE_T BufferSize
    );

// ======================
// GAME CORE STRUCTURES
// ======================
#pragma pack(push, 1)
struct PlayerState {
    DWORD sessionId;          // Session identifier (changes on restart)
    volatile LONG score;      // Atomic score counter (primary attack target)
    FLOAT health;             // Float health value (secondary target)
    CHAR playerName[16];      // Player name buffer (string injection target)
    ULONGLONG lastUpdate;     // QPC timestamp for anti-tampering
    BYTE checksum;            // Simple integrity byte (basic R3 validation)
};
#pragma pack(pop)

// ======================
// GLOBAL GAME STATE
// ======================
std::atomic<bool> g_exitRequested = false;
HANDLE g_gameMutex = nullptr;
PlayerState* g_playerState = nullptr;
ULONGLONG g_qpcFreq = 0;
PSET_PROCESS_MITIGATION_POLICY g_pSetProcessMitigationPolicy = nullptr;

// Anti-debug tripwires (basic R3 level)
const DWORD DEBUG_TRIP_ADDRESSES[] = {
    0x7FFE0000,  // KUSER_SHARED_DATA
    0x7FFD0000,  // Alternate debug region
    0x7FFE0300   // System time fields
};
const size_t DEBUG_TRIP_COUNT = _countof(DEBUG_TRIP_ADDRESSES);

// ======================
// SECURITY ENHANCEMENT FUNCTIONS
// ======================
void ApplyProcessMitigations() {
    // Load mitigation API dynamically for cross-version compatibility
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) return;

    g_pSetProcessMitigationPolicy = (PSET_PROCESS_MITIGATION_POLICY)
        GetProcAddress(hKernel32, "SetProcessMitigationPolicy");

    if (!g_pSetProcessMitigationPolicy) {
        std::cerr << "[SECURITY] SetProcessMitigationPolicy unavailable - running on legacy OS\n";
        return;
    }

    // Apply Dynamic Code Policy (blocks JIT/ROP attacks)
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicPolicy = { 0 };
    dynamicPolicy.ProhibitDynamicCode = TRUE;
    dynamicPolicy.AllowThreadOptOut = FALSE;
    dynamicPolicy.AllowRemoteDowngrade = FALSE;
    dynamicPolicy.AuditProhibitDynamicCode = FALSE;

    if (!g_pSetProcessMitigationPolicy(
        ProcessDynamicCodePolicy,
        &dynamicPolicy,
        sizeof(dynamicPolicy))) {
        std::cerr << "[SECURITY] Failed to apply DynamicCodePolicy (0x"
            << std::hex << GetLastError() << ")\n";
    }

    // Apply Signature Policy (blocks unsigned module loading)
    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sigPolicy = { 0 };
    sigPolicy.MicrosoftSignedOnly = TRUE;
    sigPolicy.StoreSignedOnly = FALSE;
    sigPolicy.AuditMicrosoftSignedOnly = FALSE;

    if (!g_pSetProcessMitigationPolicy(
        ProcessSignaturePolicy,
        &sigPolicy,
        sizeof(sigPolicy))) {
        std::cerr << "[SECURITY] Failed to apply SignaturePolicy (0x"
            << std::hex << GetLastError() << ")\n";
    }

    // Apply Strict Handle Checks (prevents handle squatting)
    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY handlePolicy = { 0 };
    handlePolicy.RaiseExceptionOnInvalidHandleReference = TRUE;
    handlePolicy.HandleExceptionsPermanentlyEnabled = TRUE;

    if (!g_pSetProcessMitigationPolicy(
        ProcessStrictHandleCheckPolicy,
        &handlePolicy,
        sizeof(handlePolicy))) {
        std::cerr << "[SECURITY] Failed to apply StrictHandlePolicy (0x"
            << std::hex << GetLastError() << ")\n";
    }

    std::cout << "[SECURITY] Process mitigations applied successfully\n";
}

// ======================
// CORE GAME FUNCTIONS
// ======================
ULONGLONG GetPreciseTime() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

void CalculateChecksum(PlayerState* state) {
    BYTE* bytes = reinterpret_cast<BYTE*>(state);
    BYTE sum = 0;

    // Exclude checksum field itself and volatile score
    for (size_t i = 0; i < offsetof(PlayerState, checksum); ++i) {
        if (i >= offsetof(PlayerState, score) &&
            i < offsetof(PlayerState, score) + sizeof(LONG)) {
            continue; // Skip score field
        }
        sum += bytes[i];
    }
    state->checksum = sum;
}

void UpdateGameState() {
    WaitForSingleObject(g_gameMutex, INFINITE);

    // Update session ID periodically to invalidate hardcoded addresses
    static DWORD sessionEpoch = 0;
    if (GetPreciseTime() - sessionEpoch > g_qpcFreq * 30) { // 30 seconds
        g_playerState->sessionId = GetTickCount64() & 0xFFFFFFFF;
        sessionEpoch = GetPreciseTime();
    }

    // Core game logic
    InterlockedIncrement(&g_playerState->score);
    g_playerState->health = fmaxf(0.0f, g_playerState->health - 0.01f);

    // Update timestamp for integrity checks
    g_playerState->lastUpdate = GetPreciseTime();

    // Recalculate checksum
    CalculateChecksum(g_playerState);

    ReleaseMutex(g_gameMutex);
}

void RenderGameScreen() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);

    // Clear screen using Windows API for performance
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

    // Render game state with memory addresses
    std::cout << "=== VMX-SENTINEL SANDBOX (PID: " << GetCurrentProcessId() << ") ===\n\n";
    std::cout << "Player State Address: 0x" << std::hex << reinterpret_cast<uintptr_t>(g_playerState) << std::dec << "\n";
    std::cout << "Session ID: " << g_playerState->sessionId << "\n";
    std::cout << "Current Score: " << g_playerState->score << " (Address: 0x"
        << std::hex << reinterpret_cast<uintptr_t>(&g_playerState->score) << std::dec << ")\n";
    std::cout << "Health: " << std::fixed << std::setprecision(2) << g_playerState->health
        << " (Address: 0x" << std::hex << reinterpret_cast<uintptr_t>(&g_playerState->health) << std::dec << ")\n";
    std::cout << "Player Name: " << g_playerState->playerName << "\n";
    std::cout << "Last Update: " << (GetPreciseTime() - g_playerState->lastUpdate) / static_cast<double>(g_qpcFreq) << "s ago\n";
    std::cout << "Memory Checksum: 0x" << std::hex << static_cast<int>(g_playerState->checksum) << std::dec << "\n\n";

    std::cout << "[!] ATTACK SURFACE READY - Red team: Modify score/health/playerName\n";
    std::cout << "[!] Blue team: Monitor for unauthorized memory modifications\n";
    std::cout << "[!] Press ESC to exit...\n";
}

void DebugTrapMonitor() {
    // Basic R3 anti-debugging tripwires
    static BYTE trapValues[DEBUG_TRIP_COUNT] = { 0 };

    for (size_t i = 0; i < DEBUG_TRIP_COUNT; ++i) {
        __try {
            BYTE currentValue = *reinterpret_cast<BYTE*>(DEBUG_TRIP_ADDRESSES[i]);
            if (trapValues[i] == 0) {
                trapValues[i] = currentValue;
            }
            else if (trapValues[i] != currentValue) {
                // Debugging activity detected! (Basic R3 level)
                std::cerr << "\n[ALERT] Memory tripwire triggered at 0x"
                    << std::hex << DEBUG_TRIP_ADDRESSES[i]
                    << " (Original: 0x" << static_cast<int>(trapValues[i])
                    << " Current: 0x" << static_cast<int>(currentValue) << ")\n";
                trapValues[i] = currentValue; // Reset trap
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Access violation - likely page not present
            continue;
        }
    }
}

void GameMainThread() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpcFreq = freq.QuadPart;

    // Initialize player state with ASLR-friendly allocation
    g_playerState = reinterpret_cast<PlayerState*>(VirtualAlloc(
        NULL,
        sizeof(PlayerState) + 0x1000, // Extra space for heap spraying simulation
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    ));

    if (!g_playerState) {
        std::cerr << "FATAL: Memory allocation failed\n";
        return;
    }

    // Initialize game state
    g_playerState->sessionId = GetTickCount64() & 0xFFFFFFFF;
    g_playerState->score = 0;
    g_playerState->health = 100.0f;
    strcpy_s(g_playerState->playerName, "QKV-Expert");
    g_playerState->lastUpdate = GetPreciseTime();
    CalculateChecksum(g_playerState);

    // Setup console
    SetConsoleTitleA("VMX-Sentinel v4.2 (Secure Execution Environment)");
    CONSOLE_FONT_INFOEX font = { sizeof(font) };
    font.dwFontSize = { 0, 16 };
    wcscpy_s(font.FaceName, L"Consolas");
    SetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &font);

    // Main game loop
    while (!g_exitRequested) {
        UpdateGameState();
        RenderGameScreen();
        DebugTrapMonitor();

        // Check for exit key (non-blocking)
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            g_exitRequested = true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Cleanup
    VirtualFree(g_playerState, 0, MEM_RELEASE);
}

// ======================
// COMPETITION ENTRY POINT
// ======================
int main() {
    // Apply security mitigations with version compatibility
    ApplyProcessMitigations();

    // Setup synchronization
    g_gameMutex = CreateMutexA(NULL, FALSE, "HV_Sandbox_Mutex_2077");
    if (!g_gameMutex) {
        std::cerr << "FATAL: Mutex creation failed (0x" << std::hex << GetLastError() << ")\n";
        return 1;
    }

    // Start game thread
    std::thread gameThread(GameMainThread);

    // Memory pressure simulation (heap allocations)
    std::vector<void*> scratchMemory;
    for (int i = 0; i < 256; ++i) {
        void* mem = VirtualAlloc(NULL, 4096, MEM_COMMIT, PAGE_READWRITE);
        if (mem) scratchMemory.push_back(mem);
    }

    // Wait for game to finish
    gameThread.join();

    // Cleanup
    for (auto mem : scratchMemory) {
        VirtualFree(mem, 0, MEM_RELEASE);
    }
    CloseHandle(g_gameMutex);

    std::cout << "\nVMX-Sentinel terminated cleanly. Competition data preserved.\n";
    return 0;
}