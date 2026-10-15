#include "stdafx.h"
#include "version.h"
#include "helper.hpp"

#ifdef _DEBUG
    #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
#else
    #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#endif

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <safetyhook.hpp>
#include <inipp/inipp.h>
#include <filesystem>
#include <fstream>
#include <cmath>

HMODULE exeModule = GetModuleHandle(NULL);
HMODULE thisModule;
HMODULE gameAssemblyModule = NULL;
HMODULE unityPlayerModule = NULL;

std::shared_ptr<spdlog::logger> logger;
std::string sLogFile = "OriARFix.log";
std::filesystem::path ExePath;
std::string sConfigFile = "OriARFix.ini";
std::filesystem::path FixPath;
inipp::Ini<char> ini;

float fTargetWidth = 4.0f;
float fTargetHeight = 3.0f;
float fTargetRatio = 1.3333333f;

bool bMaintainFOV = true;
float fFOVScale = 1.0f;

struct UnityRect { float x, y, width, height; };
struct Matrix4x4 {
    float m00, m10, m20, m30;
    float m01, m11, m21, m31;
    float m02, m12, m22, m32;
    float m03, m13, m23, m33;
};

typedef void (*Camera_set_rect_t)(void* camera, UnityRect* rect, void* method_info);
typedef void (*Camera_get_projectionMatrix_t)(Matrix4x4* out_matrix, void* camera, void* method_info);
typedef void (*Camera_set_projectionMatrix_t)(void* camera, Matrix4x4* matrix, void* method_info);
typedef float (*Camera_get_fieldOfView_t)(void* camera, void* method_info);
typedef float (*Camera_get_orthographicSize_t)(void* camera, void* method_info);

Camera_set_rect_t Camera_set_rect = nullptr;
Camera_get_projectionMatrix_t Camera_get_projectionMatrix = nullptr;
Camera_set_projectionMatrix_t Camera_set_projectionMatrix = nullptr;
Camera_get_fieldOfView_t Camera_get_fieldOfView = nullptr;
Camera_get_orthographicSize_t Camera_get_orthographicSize = nullptr;

safetyhook::InlineHook LateUpdateHook;

template<typename T>
T GetConfigAndLog(inipp::Ini<char>& ini, const std::string& section, const std::string& key, T defaultValue) {
    T value = defaultValue;
    inipp::get_value(ini.sections[section], key, value);
    SPDLOG_INFO("Config Parse: [{}]: {} = {}", section, key, value);
    return value;
}

void Configuration() {
    std::ifstream iniFile(FixPath / sConfigFile);
    if (!iniFile) return;
    ini.parse(iniFile);
    
    fTargetWidth = GetConfigAndLog(ini, "AspectRatio", "TargetWidth", 4.0f);
    fTargetHeight = GetConfigAndLog(ini, "AspectRatio", "TargetHeight", 3.0f);
    if (fTargetHeight > 0.001f) fTargetRatio = fTargetWidth / fTargetHeight;
    
    bMaintainFOV = GetConfigAndLog(ini, "FOV", "MaintainFOV", true);
    fFOVScale = GetConfigAndLog(ini, "FOV", "FOVScale", 1.0f);
}

void Logging() {
    WCHAR dllPath[_MAX_PATH] = {0};
    GetModuleFileNameW(thisModule, dllPath, MAX_PATH);
    FixPath = dllPath;
    FixPath = FixPath.remove_filename();
    
    WCHAR gameExePath[_MAX_PATH] = {0};
    GetModuleFileNameW(exeModule, gameExePath, MAX_PATH);
    ExePath = gameExePath;
    ExePath = ExePath.remove_filename();

    try {
        std::ofstream file(ExePath / sLogFile, std::ios::trunc);
        if (file.is_open()) file.close();
        logger = std::make_shared<spdlog::logger>("OriARFix", std::make_shared<spdlog::sinks::rotating_file_sink_st>((ExePath / sLogFile).string(), 10 * 1024 * 1024, 1));
        spdlog::set_default_logger(logger);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%l] %v");
        spdlog::flush_on(spdlog::level::debug);
        spdlog::set_level(spdlog::level::debug); 
        SPDLOG_INFO("Ori AR Fix (Absolute Matrix 2D/3D Edition) Initialized");
    } catch (...) {}
}

void PatchUIAspectRatio(HMODULE module, const char* moduleName) {
    if (!module) return;
    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)module;
    PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)((BYTE*)module + dosHeader->e_lfanew);
    PIMAGE_SECTION_HEADER sectionHeader = IMAGE_FIRST_SECTION(ntHeaders);

    BYTE pattern[] = { 0x39, 0x8E, 0xE3, 0x3F }; 
    BYTE patch[4];
    memcpy(patch, &fTargetRatio, 4);

    int patchCount = 0;
    for (int i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++, sectionHeader++) {
        if (sectionHeader->Characteristics & IMAGE_SCN_MEM_READ) {
            BYTE* startAddress = (BYTE*)module + sectionHeader->VirtualAddress;
            DWORD size = sectionHeader->Misc.VirtualSize;
            if (size < 4) continue;

            for (DWORD j = 0; j < size - 4; j += 4) { 
                if (memcmp(startAddress + j, pattern, 4) == 0) {
                    DWORD oldProtect;
                    if (VirtualProtect(startAddress + j, 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                        memcpy(startAddress + j, patch, 4);
                        VirtualProtect(startAddress + j, 4, oldProtect, &oldProtect);
                        patchCount++;
                    }
                }
            }
        }
    }
    SPDLOG_INFO("Ori AR Fix: Patched {} UI ratio instances in {}", patchCount, moduleName);
}

void __fastcall Hooked_LateUpdate(void* this_ptr, void* method_info) {
    if (!this_ptr) return;
    uintptr_t cameraOffset = 0x30; 
    void* m_camera = *(void**)((uintptr_t)this_ptr + cameraOffset);
    if (!m_camera) return;

    if (Camera_set_rect) {
        UnityRect fullScreen = { 0.0f, 0.0f, 1.0f, 1.0f };
        Camera_set_rect(m_camera, &fullScreen, nullptr);
    }

    if (bMaintainFOV && Camera_get_projectionMatrix && Camera_set_projectionMatrix) {
        Matrix4x4 mat;
        Camera_get_projectionMatrix(&mat, m_camera, nullptr);

        if (std::abs(mat.m33) < 0.01f && Camera_get_fieldOfView) {
            float fov = Camera_get_fieldOfView(m_camera, nullptr);
            float fovRad = fov * 0.5f * 0.0174532924f; 
            float m11_orig = 1.0f / std::tan(fovRad);

            mat.m11 = m11_orig * (fTargetRatio / 1.7777778f) * fFOVScale;
            mat.m00 = mat.m11 / fTargetRatio;
            Camera_set_projectionMatrix(m_camera, &mat, nullptr);
        }
        else if (std::abs(mat.m33 - 1.0f) < 0.01f && Camera_get_orthographicSize) {
            float orthoSize = Camera_get_orthographicSize(m_camera, nullptr);
            
            if (orthoSize > 0.001f) {
                float m11_orig = 1.0f / orthoSize;
                float uiScale = fTargetRatio / 1.7777778f; 
                
                mat.m11 = m11_orig * uiScale;
                mat.m00 = mat.m11 / fTargetRatio;
                Camera_set_projectionMatrix(m_camera, &mat, nullptr);
            }
        }
    }
}

void ApplyHooks() {
    uintptr_t baseAddr = (uintptr_t)gameAssemblyModule;

    Camera_set_rect = (Camera_set_rect_t)(baseAddr + 0x2422D20);
    Camera_get_projectionMatrix = (Camera_get_projectionMatrix_t)(baseAddr + 0x2423520);
    Camera_set_projectionMatrix = (Camera_set_projectionMatrix_t)(baseAddr + 0x24235D0);
    Camera_get_fieldOfView = (Camera_get_fieldOfView_t)(baseAddr + 0x24211B0);
    Camera_get_orthographicSize = (Camera_get_orthographicSize_t)(baseAddr + 0x2421670);

    uintptr_t lateUpdateRVA = 0x9F7780;
    uint8_t* lateUpdateAddr = (uint8_t*)(baseAddr + lateUpdateRVA);

    if (lateUpdateAddr) {
        SPDLOG_INFO("Ori AR Fix: Injecting Ultimate Matrix Hook at 0x{:X}", lateUpdateRVA);
        LateUpdateHook = safetyhook::create_inline(lateUpdateAddr, Hooked_LateUpdate);
    }

    PatchUIAspectRatio(gameAssemblyModule, "GameAssembly.dll");
    PatchUIAspectRatio(unityPlayerModule, "UnityPlayer.dll");
}

DWORD __stdcall Main(void*) {
    Logging();
    Configuration();
    while (!gameAssemblyModule || !unityPlayerModule) {
        if (!gameAssemblyModule) gameAssemblyModule = GetModuleHandleA("GameAssembly.dll");
        if (!unityPlayerModule) unityPlayerModule = GetModuleHandleA("UnityPlayer.dll");
        Sleep(1000);
    }
    Sleep(5000); 
    ApplyHooks();
    return true;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        thisModule = hModule;
        if (HANDLE mainHandle = CreateThread(NULL, 0, Main, 0, NULL, 0)) {
            SetThreadPriority(mainHandle, THREAD_PRIORITY_HIGHEST);
            CloseHandle(mainHandle);
        }
    }
    return TRUE;
}