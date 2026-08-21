// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Vulkan/GSLsfg.h"

#include "Config.h"
#include "GS/GS.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <atomic>
#include <cstdio>
#include <cstring>

// --- Android API level helpers (available from API 24) ---
#if __ANDROID_API__ >= 24
#include <android/api-level.h>
#else
// Dummy for older builds; we never call it.
static inline int android_get_device_api_level() { return __ANDROID_API__; }
#endif

// --- Compile-time availability of the LSFG implementation ---
// LSFG requires AHardwareBuffer (API 26+) and associated Vulkan extensions.
#if defined(ARMSX2_HAS_LSFG) && (__ANDROID_API__ >= 26)
#define LSFG_IMPLEMENTATION_AVAILABLE 1
#else
#undef ARMSX2_HAS_LSFG
#define ARMSX2_HAS_LSFG 0
#endif

// If the implementation is not available, the functions that depend on it are stubbed.
// The public interface (state queries, etc.) remains fully functional.

namespace GSLsfg
{
    namespace
    {
        std::string s_dll_path;

        // Written once from the GS thread at device creation, read from the UI thread.
        std::atomic<bool> s_caps_known{false};
        std::atomic<bool> s_is_vulkan{false};
        std::atomic<u32> s_adreno_generation{0};

        std::atomic<bool> s_init_failed{false};
        std::atomic<bool> s_dll_checked{false};
        std::atomic<bool> s_dll_ok{false};

        std::atomic<float> s_display_fps{0.0f};
        std::atomic<bool> s_no_shaders{false};
    } // namespace

    void NoteRendererCapability(bool is_vulkan, u32 adreno_generation)
    {
        s_is_vulkan.store(is_vulkan, std::memory_order_relaxed);
        s_adreno_generation.store(adreno_generation, std::memory_order_relaxed);
        s_caps_known.store(true, std::memory_order_release);
    }

    void SetDllPath(std::string path)
    {
        if (s_dll_path == path)
            return;
        s_dll_path = std::move(path);
        s_init_failed.store(false, std::memory_order_relaxed);
        s_dll_checked.store(false, std::memory_order_relaxed);
        s_no_shaders.store(false, std::memory_order_relaxed);
    }

    const std::string& GetDllPath() { return s_dll_path; }

    bool LooksLikeLosslessDll(const std::string& path)
    {
        auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
        if (!fp)
            return false;

        u8 dos[0x40] = {};
        if (std::fread(dos, sizeof(dos), 1, fp.get()) != 1 || dos[0] != 'M' || dos[1] != 'Z')
            return false;

        const u32 pe_off = static_cast<u32>(dos[0x3C]) | (static_cast<u32>(dos[0x3D]) << 8) |
                           (static_cast<u32>(dos[0x3E]) << 16) | (static_cast<u32>(dos[0x3F]) << 24);
        if (pe_off < sizeof(dos) || pe_off > (64u * 1024u * 1024u))
            return false;

        if (FileSystem::FSeek64(fp.get(), static_cast<s64>(pe_off), SEEK_SET) != 0)
            return false;
        u8 sig[4] = {};
        if (std::fread(sig, sizeof(sig), 1, fp.get()) != 1)
            return false;
        return sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0;
    }

    // --- GetUnavailableReason with runtime API check ---
    Unavailable GetUnavailableReason()
    {
#ifndef ARMSX2_HAS_LSFG
        return Unavailable::NotCompiledIn;
#else
        // Runtime check: if the device runs API < 26, we cannot use AHardwareBuffer.
        #if __ANDROID_API__ >= 24
            static const int s_api_level = android_get_device_api_level();
            if (s_api_level < 26)
                return Unavailable::AndroidTooOld;
        #endif

        // Now the implementation is available both at compile time and runtime.
        // The rest of the checks use variables that are defined in the implementation part.
        // If LSFG_IMPLEMENTATION_AVAILABLE is not defined, we fall back to NotCompiledIn.
        #ifdef LSFG_IMPLEMENTATION_AVAILABLE
            if (s_caps_known.load(std::memory_order_acquire))
            {
                if (!s_is_vulkan.load(std::memory_order_relaxed))
                    return Unavailable::NotVulkan;
                if (s_adreno_generation.load(std::memory_order_relaxed) < 7)
                    return Unavailable::GpuUnsupported;
            }

            if (s_dll_path.empty())
                return Unavailable::NoDll;
            if (!s_dll_checked.load(std::memory_order_acquire))
            {
                s_dll_ok.store(LooksLikeLosslessDll(s_dll_path), std::memory_order_relaxed);
                s_dll_checked.store(true, std::memory_order_release);
            }
            if (!s_dll_ok.load(std::memory_order_relaxed))
                return Unavailable::DllUnreadable;
            if (s_init_failed.load(std::memory_order_relaxed))
                return Unavailable::InitFailed;
            return Unavailable::Available;
        #else
            return Unavailable::NotCompiledIn;
        #endif
#endif
    }

    bool IsAvailable() { return GetUnavailableReason() == Unavailable::Available; }

    const char* GetUnavailableReasonString()
    {
        switch (GetUnavailableReason())
        {
            case Unavailable::Available: return "available";
            case Unavailable::NotCompiledIn: return "not included in this build";
            case Unavailable::NotVulkan: return "requires the Vulkan renderer";
            case Unavailable::GpuUnsupported: return "requires an Adreno 7xx or newer GPU";
            case Unavailable::NoDll: return "no Lossless.dll selected";
            case Unavailable::DllUnreadable: return "the selected file is not a readable DLL";
            case Unavailable::InitFailed: return "frame generation failed to start on this device";
            case Unavailable::AndroidTooOld: return "Android 8.0 (API 26) or newer required";
            default: return "unavailable";
        }
    }

    float GetDisplayFPS() { return s_display_fps.load(std::memory_order_relaxed); }

    std::string GetStatusText()
    {
        if (!GSConfig.LsfgEnabled)
            return {};

        switch (GetUnavailableReason())
        {
            case Unavailable::Available:
                break;
            case Unavailable::InitFailed:
                return s_no_shaders.load(std::memory_order_relaxed) ? "LSFG: no shaders" : "LSFG: failed";
            default:
                return "LSFG: unavailable";
        }

        const float fps = s_display_fps.load(std::memory_order_relaxed);
        if (fps <= 0.0f)
            return "LSFG: starting";
        return fmt::format("LSFG: {:.2f}", fps);
    }
} // namespace GSLsfg

// ============================================================================
// Implementation part – only compiled when LSFG_IMPLEMENTATION_AVAILABLE = 1
// ============================================================================

#ifdef LSFG_IMPLEMENTATION_AVAILABLE

#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/VKSwapChain.h"

#include "armsx2_lsfg_shim.h"
#include "extract/trans.hpp"

#include <pe-parse/parse.h>

#include <android/hardware_buffer.h>
#include <dlfcn.h>

#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace GSLsfg
{
    namespace
    {
        // --- backend loading (dlopen) ---
        struct Backend
        {
            void* handle = nullptr;
            pfn_armsx2_lsfg_abi_version abi_version = nullptr;
            pfn_armsx2_lsfg_initialize initialize = nullptr;
            pfn_armsx2_lsfg_create_context create_context = nullptr;
            pfn_armsx2_lsfg_present present = nullptr;
            pfn_armsx2_lsfg_wait_idle wait_idle = nullptr;
            pfn_armsx2_lsfg_delete_context delete_context = nullptr;
            pfn_armsx2_lsfg_finalize finalize = nullptr;
            pfn_armsx2_lsfg_last_error last_error = nullptr;
        };

        Backend s_backend;
        bool s_backend_tried = false;

        const char* BackendError()
        {
            const char* msg = s_backend.last_error ? s_backend.last_error() : nullptr;
            return (msg && *msg) ? msg : "no detail";
        }

        bool LoadBackend()
        {
            if (s_backend.handle)
                return true;
            if (s_backend_tried)
                return false;
            s_backend_tried = true;

            void* handle = dlopen("libarmsx2_lsfg.so", RTLD_NOW | RTLD_LOCAL);
            if (!handle)
            {
                Console.ErrorFmt("@@ANDROID_LSFG@@ libarmsx2_lsfg.so not loadable: {}", dlerror());
                return false;
            }

            Backend b;
            b.handle = handle;
            auto sym = [handle](const char* name) { return dlsym(handle, name); };
            b.abi_version = reinterpret_cast<pfn_armsx2_lsfg_abi_version>(sym("armsx2_lsfg_abi_version"));
            b.initialize = reinterpret_cast<pfn_armsx2_lsfg_initialize>(sym("armsx2_lsfg_initialize"));
            b.create_context = reinterpret_cast<pfn_armsx2_lsfg_create_context>(sym("armsx2_lsfg_create_context"));
            b.present = reinterpret_cast<pfn_armsx2_lsfg_present>(sym("armsx2_lsfg_present"));
            b.wait_idle = reinterpret_cast<pfn_armsx2_lsfg_wait_idle>(sym("armsx2_lsfg_wait_idle"));
            b.delete_context = reinterpret_cast<pfn_armsx2_lsfg_delete_context>(sym("armsx2_lsfg_delete_context"));
            b.finalize = reinterpret_cast<pfn_armsx2_lsfg_finalize>(sym("armsx2_lsfg_finalize"));
            b.last_error = reinterpret_cast<pfn_armsx2_lsfg_last_error>(sym("armsx2_lsfg_last_error"));

            if (!b.abi_version || !b.initialize || !b.create_context || !b.present || !b.wait_idle ||
                !b.delete_context || !b.finalize || !b.last_error)
            {
                Console.Error("@@ANDROID_LSFG@@ libarmsx2_lsfg.so is missing entry points");
                dlclose(handle);
                return false;
            }
            if (b.abi_version() != ARMSX2_LSFG_ABI_VERSION)
            {
                Console.ErrorFmt("@@ANDROID_LSFG@@ libarmsx2_lsfg.so is ABI v{}, expected v{}",
                    b.abi_version(), ARMSX2_LSFG_ABI_VERSION);
                dlclose(handle);
                return false;
            }

            s_backend = b;
            return true;
        }

        // --- shader extraction ---
        std::map<std::string, std::vector<u8>> s_shader_spirv;
        std::unordered_map<u32, std::vector<u8>> s_shader_blobs;
        std::string s_shader_source;
        bool s_have_standard = false;
        bool s_have_performance = false;

        int OnResource(void*, const peparse::resource& res)
        {
            if (res.type != peparse::RT_RCDATA || res.buf == nullptr || res.buf->bufLen <= 0)
                return 0;
            std::vector<u8> data(static_cast<size_t>(res.buf->bufLen));
            std::copy_n(res.buf->buf, res.buf->bufLen, data.data());
            s_shader_blobs[res.name] = std::move(data);
            return 0;
        }

        bool IsPerformanceShader(const std::string& name) { return name.compare(0, 2, "p_") == 0; }

        const std::map<std::string, u32>& ShaderNameTable()
        {
            static const std::map<std::string, u32> table = {
                {"mipmaps", 255},
                {"alpha[0]", 267}, {"alpha[1]", 268}, {"alpha[2]", 269}, {"alpha[3]", 270},
                {"beta[0]", 275}, {"beta[1]", 276}, {"beta[2]", 277}, {"beta[3]", 278},
                {"beta[4]", 279},
                {"gamma[0]", 257}, {"gamma[1]", 259}, {"gamma[2]", 260}, {"gamma[3]", 261},
                {"gamma[4]", 262},
                {"delta[0]", 257}, {"delta[1]", 263}, {"delta[2]", 264}, {"delta[3]", 265},
                {"delta[4]", 266}, {"delta[5]", 258}, {"delta[6]", 271}, {"delta[7]", 272},
                {"delta[8]", 273}, {"delta[9]", 274},
                {"generate", 256},
                {"p_mipmaps", 255},
                {"p_alpha[0]", 290}, {"p_alpha[1]", 291}, {"p_alpha[2]", 292}, {"p_alpha[3]", 293},
                {"p_beta[0]", 298}, {"p_beta[1]", 299}, {"p_beta[2]", 300}, {"p_beta[3]", 301},
                {"p_beta[4]", 302},
                {"p_gamma[0]", 280}, {"p_gamma[1]", 282}, {"p_gamma[2]", 283}, {"p_gamma[3]", 284},
                {"p_gamma[4]", 285},
                {"p_delta[0]", 280}, {"p_delta[1]", 286}, {"p_delta[2]", 287}, {"p_delta[3]", 288},
                {"p_delta[4]", 289}, {"p_delta[5]", 281}, {"p_delta[6]", 294}, {"p_delta[7]", 295},
                {"p_delta[8]", 296}, {"p_delta[9]", 297},
                {"p_generate", 256},
            };
            return table;
        }

        // --- shader cache helpers (unchanged) ---
        constexpr u32 k_cache_magic = 0x4746534Cu; // "LSFG"
        constexpr u32 k_cache_version = 1;
        constexpr u32 k_max_name_len = 64;
        constexpr u32 k_max_shader_size = 4u * 1024u * 1024u;
        constexpr u32 k_max_shader_count = 256;

        std::string ShaderCachePath() { return Path::Combine(EmuFolders::Cache, "lsfg_shaders.bin"); }

        void AppendU32(std::vector<u8>& out, u32 value)
        {
            out.insert(out.end(), reinterpret_cast<const u8*>(&value), reinterpret_cast<const u8*>(&value) + 4);
        }

        void AppendU64(std::vector<u8>& out, u64 value)
        {
            out.insert(out.end(), reinterpret_cast<const u8*>(&value), reinterpret_cast<const u8*>(&value) + 8);
        }

        bool StatSourceDll(u64* size, u64* mtime)
        {
            FILESYSTEM_STAT_DATA sd = {};
            if (!FileSystem::StatFile(s_dll_path.c_str(), &sd))
                return false;
            *size = static_cast<u64>(sd.Size);
            *mtime = static_cast<u64>(sd.ModificationTime);
            return true;
        }

        void SaveShaderCache()
        {
            u64 dll_size = 0, dll_mtime = 0;
            if (!StatSourceDll(&dll_size, &dll_mtime))
                return;

            std::vector<u8> out;
            AppendU32(out, k_cache_magic);
            AppendU32(out, k_cache_version);
            AppendU64(out, dll_size);
            AppendU64(out, dll_mtime);
            AppendU32(out, static_cast<u32>(s_shader_spirv.size()));
            for (const auto& [name, spirv] : s_shader_spirv)
            {
                AppendU32(out, static_cast<u32>(name.size()));
                out.insert(out.end(), name.begin(), name.end());
                AppendU32(out, static_cast<u32>(spirv.size()));
                out.insert(out.end(), spirv.begin(), spirv.end());
            }

            const std::string path = ShaderCachePath();
            if (!FileSystem::WriteBinaryFile(path.c_str(), out.data(), out.size()))
            {
                Console.WarningFmt("@@ANDROID_LSFG@@ could not write {} — shaders will be translated again next time", path);
                return;
            }
            Console.WriteLnFmt("@@ANDROID_LSFG@@ cached {} translated shaders", s_shader_spirv.size());
        }

        bool LoadShaderCache()
        {
            u64 dll_size = 0, dll_mtime = 0;
            if (!StatSourceDll(&dll_size, &dll_mtime))
                return false;

            const std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(ShaderCachePath().c_str());
            if (!data.has_value())
                return false;

            const u8* p = data->data();
            size_t left = data->size();
            const auto read_u32 = [&p, &left](u32* value) {
                if (left < 4)
                    return false;
                std::memcpy(value, p, 4);
                p += 4;
                left -= 4;
                return true;
            };
            const auto read_u64 = [&p, &left](u64* value) {
                if (left < 8)
                    return false;
                std::memcpy(value, p, 8);
                p += 8;
                left -= 8;
                return true;
            };

            u32 magic = 0, version = 0, count = 0;
            u64 cached_size = 0, cached_mtime = 0;
            if (!read_u32(&magic) || !read_u32(&version) || !read_u64(&cached_size) ||
                !read_u64(&cached_mtime) || !read_u32(&count))
                return false;
            if (magic != k_cache_magic || version != k_cache_version)
                return false;
            if (cached_size != dll_size || cached_mtime != dll_mtime)
            {
                Console.WriteLn("@@ANDROID_LSFG@@ Lossless.dll changed since the shader cache was written");
                return false;
            }
            if (count == 0 || count > k_max_shader_count)
                return false;

            std::map<std::string, std::vector<u8>> loaded;
            for (u32 i = 0; i < count; i++)
            {
                u32 name_len = 0, size = 0;
                if (!read_u32(&name_len) || name_len == 0 || name_len > k_max_name_len || left < name_len)
                    break;
                std::string name(reinterpret_cast<const char*>(p), name_len);
                p += name_len;
                left -= name_len;

                if (!read_u32(&size) || size == 0 || size > k_max_shader_size || left < size)
                    break;
                loaded[std::move(name)].assign(p, p + size);
                p += size;
                left -= size;
            }

            if (loaded.size() != count)
            {
                Console.Warning("@@ANDROID_LSFG@@ shader cache is truncated — translating again");
                return false;
            }

            s_shader_spirv = std::move(loaded);
            Console.WriteLnFmt("@@ANDROID_LSFG@@ restored {} shaders from the cache", s_shader_spirv.size());
            return true;
        }

        void ClassifyShaderFamilies()
        {
            s_have_standard = true;
            s_have_performance = true;
            for (const auto& [name, idx] : ShaderNameTable())
            {
                if (s_shader_spirv.find(name) != s_shader_spirv.end())
                    continue;
                if (IsPerformanceShader(name))
                    s_have_performance = false;
                else
                    s_have_standard = false;
            }
        }

        void ExtractShaders()
        {
            if (s_shader_source != s_dll_path)
                s_shader_spirv.clear();
            if (!s_shader_spirv.empty())
                return;

            s_shader_source = s_dll_path;
            const bool from_cache = LoadShaderCache();
            if (!from_cache)
            {
                s_shader_blobs.clear();
                peparse::parsed_pe* dll = peparse::ParsePEFromFile(s_dll_path.c_str());
                if (!dll)
                    throw std::runtime_error("could not read Lossless.dll");
                peparse::IterRsrc(dll, OnResource, nullptr);
                peparse::DestructParsedPE(dll);

                for (const auto& [name, idx] : ShaderNameTable())
                {
                    const auto blob = s_shader_blobs.find(idx);
                    if (blob == s_shader_blobs.end())
                        continue;
                    try
                    {
                        std::vector<u8> spirv = Extract::translateShader(blob->second);
                        if (!spirv.empty())
                            s_shader_spirv[name] = std::move(spirv);
                    }
                    catch (const std::exception& ex)
                    {
                        Console.ErrorFmt("@@ANDROID_LSFG@@ shader '{}' failed to translate: {}", name, ex.what());
                    }
                }
                s_shader_blobs.clear();
            }

            ClassifyShaderFamilies();
            if (!s_have_standard && !s_have_performance)
            {
                s_shader_spirv.clear();
                s_shader_source.clear();
                s_no_shaders.store(true, std::memory_order_relaxed);
                throw std::runtime_error(
                    "Lossless.dll has no complete shader set — is Lossless Scaling up to date?");
            }
            s_no_shaders.store(false, std::memory_order_relaxed);
            if (!from_cache)
                SaveShaderCache();
        }

        int ShaderCallback(void*, const char* name, const uint8_t** out_data, uint32_t* out_size)
        {
            try
            {
                const auto hit = s_shader_spirv.find(name);
                if (hit == s_shader_spirv.end() || hit->second.empty())
                {
                    Console.ErrorFmt(
                        "@@ANDROID_LSFG@@ framegen asked for shader '{}', which this DLL does not have", name);
                    return -1;
                }
                *out_data = hit->second.data();
                *out_size = static_cast<uint32_t>(hit->second.size());
                return 0;
            }
            catch (...)
            {
                return -1;
            }
        }

        // --- AHardwareBuffer-backed images ---
        struct AhbImage
        {
            AHardwareBuffer* ahb = nullptr;
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
        };

        VkDevice s_device = VK_NULL_HANDLE;
        VkPhysicalDevice s_physical_device = VK_NULL_HANDLE;
        VkQueue s_queue = VK_NULL_HANDLE;
        VkCommandPool s_cmd_pool = VK_NULL_HANDLE;

        AhbImage s_frame[2];
        std::vector<AhbImage> s_generated;

        s32 s_context_id = -1;
        bool s_active = false;
        u32 s_multiplier = 1;
        bool s_performance_requested = false;
        u8 s_flow_scale_percent = 100;
        VkExtent2D s_extent = {};
        VkFormat s_format = VK_FORMAT_UNDEFINED;
        u64 s_frame_index = 0;

        u64 s_fps_window_start = 0;
        u32 s_fps_real = 0;
        u32 s_fps_generated = 0;

        void NoteFramesDisplayed(u32 real, u32 generated)
        {
            s_fps_real += real;
            s_fps_generated += generated;

            const u64 now = Common::Timer::GetCurrentValue();
            if (s_fps_window_start == 0)
            {
                s_fps_window_start = now;
                return;
            }
            const double secs = Common::Timer::ConvertValueToSeconds(now - s_fps_window_start);
            if (secs < 1.0)
                return;

            s_display_fps.store(static_cast<float>((s_fps_real + s_fps_generated) / secs), std::memory_order_relaxed);
            s_fps_window_start = now;
            s_fps_real = 0;
            s_fps_generated = 0;
        }

        VkCommandBuffer s_pre_copy_cmd = VK_NULL_HANDLE;
        VkSemaphore s_pre_copy_sem = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> s_post_copy_cmds;
        std::vector<VkSemaphore> s_post_copy_sems;
        std::vector<VkSemaphore> s_acquire_sems;

        void DestroyAhbImage(AhbImage& img)
        {
            if (img.image != VK_NULL_HANDLE)
                vkDestroyImage(s_device, img.image, nullptr);
            if (img.memory != VK_NULL_HANDLE)
                vkFreeMemory(s_device, img.memory, nullptr);
            if (img.ahb)
                AHardwareBuffer_release(img.ahb);
            img = {};
        }

        bool CreateAhbImage(AhbImage& out, VkExtent2D extent, VkFormat format)
        {
            u32 ahb_format = 0;
            switch (format)
            {
                case VK_FORMAT_R8G8B8A8_UNORM: ahb_format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM; break;
                case VK_FORMAT_R16G16B16A16_SFLOAT: ahb_format = AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT; break;
                default:
                    Console.ErrorFmt("@@ANDROID_LSFG@@ unsupported swapchain format {}", static_cast<u32>(format));
                    return false;
            }

            const AHardwareBuffer_Desc desc = {
                extent.width, extent.height, 1, ahb_format,
                AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
                0, 0, 0};
            if (AHardwareBuffer_allocate(&desc, &out.ahb) != 0 || !out.ahb)
            {
                Console.Error("@@ANDROID_LSFG@@ AHardwareBuffer_allocate failed");
                return false;
            }

            VkExternalMemoryImageCreateInfo ext_info = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
            VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext_info, 0, VK_IMAGE_TYPE_2D,
                format, {extent.width, extent.height, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
            if (vkCreateImage(s_device, &image_info, nullptr, &out.image) != VK_SUCCESS)
            {
                Console.Error("@@ANDROID_LSFG@@ vkCreateImage failed for the shared image");
                DestroyAhbImage(out);
                return false;
            }

            VkMemoryRequirements reqs = {};
            vkGetImageMemoryRequirements(s_device, out.image, &reqs);

            VkPhysicalDeviceMemoryProperties mem_props = {};
            vkGetPhysicalDeviceMemoryProperties(s_physical_device, &mem_props);

            u32 type_index = UINT32_MAX;
            for (u32 i = 0; i < mem_props.memoryTypeCount; i++)
            {
                if ((reqs.memoryTypeBits & (1u << i)) &&
                    (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                {
                    type_index = i;
                    break;
                }
            }
            if (type_index == UINT32_MAX)
            {
                for (u32 i = 0; i < mem_props.memoryTypeCount; i++)
                {
                    if (reqs.memoryTypeBits & (1u << i))
                    {
                        type_index = i;
                        break;
                    }
                }
            }
            if (type_index == UINT32_MAX)
            {
                Console.Error("@@ANDROID_LSFG@@ no memory type accepts the shared image");
                DestroyAhbImage(out);
                return false;
            }

            VkMemoryDedicatedAllocateInfo dedicated = {
                VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, out.image, VK_NULL_HANDLE};
            VkImportAndroidHardwareBufferInfoANDROID import_info = {
                VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &dedicated, out.ahb};
            VkMemoryAllocateInfo alloc_info = {
                VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import_info, reqs.size, type_index};
            if (vkAllocateMemory(s_device, &alloc_info, nullptr, &out.memory) != VK_SUCCESS)
            {
                Console.Error("@@ANDROID_LSFG@@ could not import the AHardwareBuffer");
                DestroyAhbImage(out);
                return false;
            }
            if (vkBindImageMemory(s_device, out.image, out.memory, 0) != VK_SUCCESS)
            {
                Console.Error("@@ANDROID_LSFG@@ vkBindImageMemory failed for the shared image");
                DestroyAhbImage(out);
                return false;
            }
            return true;
        }

        // --- copy helpers (unchanged) ---
        void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
            VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
            VkPipelineStageFlags dst_stage)
        {
            const VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, src_access,
                dst_access, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        void RecordCopy(VkCommandBuffer cmd, VkImage src, VkImageLayout src_layout, VkImage dst,
            VkImageLayout dst_layout, VkExtent2D extent)
        {
            ImageBarrier(cmd, src, src_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

            const VkImageCopy region = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {extent.width, extent.height, 1}};
            vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            ImageBarrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_layout, VK_ACCESS_TRANSFER_READ_BIT,
                0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_layout, VK_ACCESS_TRANSFER_WRITE_BIT,
                0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }

        bool SubmitOneShot(VkCommandBuffer cmd, VkSemaphore wait, VkSemaphore signal)
        {
            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.waitSemaphoreCount = (wait != VK_NULL_HANDLE) ? 1u : 0u;
            submit.pWaitSemaphores = (wait != VK_NULL_HANDLE) ? &wait : nullptr;
            submit.pWaitDstStageMask = (wait != VK_NULL_HANDLE) ? &wait_stage : nullptr;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            submit.signalSemaphoreCount = (signal != VK_NULL_HANDLE) ? 1u : 0u;
            submit.pSignalSemaphores = (signal != VK_NULL_HANDLE) ? &signal : nullptr;
            return vkQueueSubmit(s_queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        }

        void DestroyResources()
        {
            if (s_device == VK_NULL_HANDLE)
                return;

            for (VkSemaphore s : s_post_copy_sems)
                if (s != VK_NULL_HANDLE)
                    vkDestroySemaphore(s_device, s, nullptr);
            for (VkSemaphore s : s_acquire_sems)
                if (s != VK_NULL_HANDLE)
                    vkDestroySemaphore(s_device, s, nullptr);
            if (s_pre_copy_sem != VK_NULL_HANDLE)
                vkDestroySemaphore(s_device, s_pre_copy_sem, nullptr);
            s_post_copy_sems.clear();
            s_acquire_sems.clear();
            s_pre_copy_sem = VK_NULL_HANDLE;

            if (s_cmd_pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(s_device, s_cmd_pool, nullptr);
            s_cmd_pool = VK_NULL_HANDLE;
            s_pre_copy_cmd = VK_NULL_HANDLE;
            s_post_copy_cmds.clear();

            for (AhbImage& img : s_generated)
                DestroyAhbImage(img);
            s_generated.clear();
            DestroyAhbImage(s_frame[0]);
            DestroyAhbImage(s_frame[1]);
        }

        bool FailInitialize(const char* why)
        {
            Console.ErrorFmt("@@ANDROID_LSFG@@ {} — frame generation off", why);
            s_init_failed.store(true, std::memory_order_relaxed);
            if (s_context_id >= 0 && s_backend.delete_context)
                s_backend.delete_context(s_context_id);
            s_context_id = -1;
            if (s_backend.finalize)
                s_backend.finalize();
            DestroyResources();
            s_device = VK_NULL_HANDLE;
            s_physical_device = VK_NULL_HANDLE;
            s_queue = VK_NULL_HANDLE;
            return false;
        }
    } // namespace

    // --- Public implementation functions ---

    bool IsActive() { return s_active; }

    u32 GetMultiplier() { return s_active ? s_multiplier : 1u; }

    void Shutdown()
    {
        if (!s_active && s_context_id < 0 && s_cmd_pool == VK_NULL_HANDLE)
            return;

        if (s_context_id >= 0)
        {
            s_backend.wait_idle();
            s_backend.delete_context(s_context_id);
        }
        s_backend.finalize();
        s_context_id = -1;

        if (s_device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(s_device);
        DestroyResources();

        s_active = false;
        s_multiplier = 1;
        s_performance_requested = false;
        s_flow_scale_percent = 100;
        s_extent = {};
        s_format = VK_FORMAT_UNDEFINED;
        s_frame_index = 0;
        s_device = VK_NULL_HANDLE;
        s_physical_device = VK_NULL_HANDLE;
        s_queue = VK_NULL_HANDLE;

        s_display_fps.store(0.0f, std::memory_order_relaxed);
        s_fps_window_start = 0;
        s_fps_real = 0;
        s_fps_generated = 0;
    }

    bool Initialize(VKSwapChain* swap_chain, u32 multiplier)
    {
        if (!swap_chain || !g_gs_device || !IsAvailable())
            return false;

        // Extra runtime guard (though GetUnavailableReason already checks API level)
        #if __ANDROID_API__ >= 24
            if (android_get_device_api_level() < 26) {
                s_init_failed.store(true, std::memory_order_relaxed);
                return false;
            }
        #endif

        if (!LoadBackend())
        {
            s_init_failed.store(true, std::memory_order_relaxed);
            return false;
        }

        multiplier = std::clamp<u32>(multiplier, 2, 4);
        const u8 flow_scale_percent = std::clamp<u8>(GSConfig.LsfgFlowScale, 25, 100);
        const VkExtent2D extent = {swap_chain->GetWidth(), swap_chain->GetHeight()};
        const VkFormat format = swap_chain->GetTextureFormat();

        if (s_active && extent.width == s_extent.width && extent.height == s_extent.height &&
            format == s_format && multiplier == s_multiplier &&
            GSConfig.LsfgPerformance == s_performance_requested && flow_scale_percent == s_flow_scale_percent)
        {
            return true;
        }
        if (s_active || s_cmd_pool != VK_NULL_HANDLE)
            Shutdown();

        if (extent.width == 0 || extent.height == 0)
            return false;

        GSDeviceVK* dev = GSDeviceVK::GetInstance();
        s_device = dev->GetDevice();
        s_physical_device = dev->GetPhysicalDevice();
        s_queue = dev->GetGraphicsQueue();

        if (!CreateAhbImage(s_frame[0], extent, format) || !CreateAhbImage(s_frame[1], extent, format))
            return FailInitialize("could not allocate the shared frame images");
        s_generated.resize(multiplier - 1);
        for (u32 i = 0; i < multiplier - 1; i++)
        {
            if (!CreateAhbImage(s_generated[i], extent, format))
                return FailInitialize("could not allocate the interpolated frame images");
        }

        const VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, dev->GetGraphicsQueueFamilyIndex()};
        if (vkCreateCommandPool(s_device, &pool_info, nullptr, &s_cmd_pool) != VK_SUCCESS)
            return FailInitialize("vkCreateCommandPool failed");

        {
            std::vector<VkCommandBuffer> buffers(multiplier);
            const VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
                s_cmd_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, multiplier};
            if (vkAllocateCommandBuffers(s_device, &alloc, buffers.data()) != VK_SUCCESS)
                return FailInitialize("vkAllocateCommandBuffers failed");
            s_pre_copy_cmd = buffers[0];
            s_post_copy_cmds.assign(buffers.begin() + 1, buffers.end());
        }

        {
            const VkSemaphoreCreateInfo sem_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            bool ok = vkCreateSemaphore(s_device, &sem_info, nullptr, &s_pre_copy_sem) == VK_SUCCESS;
            s_post_copy_sems.assign(multiplier - 1, VK_NULL_HANDLE);
            s_acquire_sems.assign(multiplier - 1, VK_NULL_HANDLE);
            for (u32 i = 0; ok && i < multiplier - 1; i++)
            {
                ok = vkCreateSemaphore(s_device, &sem_info, nullptr, &s_post_copy_sems[i]) == VK_SUCCESS &&
                     vkCreateSemaphore(s_device, &sem_info, nullptr, &s_acquire_sems[i]) == VK_SUCCESS;
            }
            if (!ok)
                return FailInitialize("vkCreateSemaphore failed");
        }

        try
        {
            ExtractShaders();
        }
        catch (const std::exception& ex)
        {
            return FailInitialize(ex.what());
        }
        catch (...)
        {
            return FailInitialize("Lossless.dll could not be read");
        }

        bool use_performance = GSConfig.LsfgPerformance;
        if (use_performance && !s_have_performance)
        {
            Console.WriteLn("@@ANDROID_LSFG@@ this Lossless.dll has no 3.1p shaders — using 3.1");
            use_performance = false;
        }
        else if (!use_performance && !s_have_standard)
        {
            Console.WriteLn("@@ANDROID_LSFG@@ this Lossless.dll has only 3.1p shaders — using 3.1p");
            use_performance = true;
        }

        const float flow_scale = std::clamp(100.0f / static_cast<float>(flow_scale_percent), 1.0f, 4.0f);

        const VkPhysicalDeviceProperties& props = dev->GetDeviceProperties();
        const u64 device_uuid = (static_cast<u64>(props.vendorID) << 32) | props.deviceID;
        if (s_backend.initialize(device_uuid, /*is_hdr*/ 0, flow_scale, multiplier - 1,
                use_performance ? 1 : 0, ShaderCallback, nullptr) != 0)
            return FailInitialize(BackendError());

        std::vector<AHardwareBuffer*> outputs;
        outputs.reserve(s_generated.size());
        for (const AhbImage& img : s_generated)
            outputs.push_back(img.ahb);

        s_context_id = s_backend.create_context(s_frame[0].ahb, s_frame[1].ahb, outputs.data(),
            static_cast<u32>(outputs.size()), extent.width, extent.height, static_cast<u32>(format));
        if (s_context_id < 0)
            return FailInitialize(BackendError());

        s_extent = extent;
        s_format = format;
        s_multiplier = multiplier;
        s_performance_requested = GSConfig.LsfgPerformance;
        s_flow_scale_percent = flow_scale_percent;
        s_frame_index = 0;
        s_active = true;
        Console.WriteLnFmt("@@ANDROID_LSFG@@ active: {}x{} x{} frames, {}, flow {}%", extent.width, extent.height,
            multiplier, use_performance ? "3.1p" : "3.1", flow_scale_percent);
        return true;
    }

    bool PresentWithGeneration(
        VkQueue present_queue, VKSwapChain* swap_chain, VkSemaphore render_finished, bool frame_has_new_content)
    {
        if (!s_active || !swap_chain)
            return false;

        if (!frame_has_new_content)
        {
            s_frame_index = 0;
            NoteFramesDisplayed(1, 0);
            return false;
        }

        if (swap_chain->GetWidth() != s_extent.width || swap_chain->GetHeight() != s_extent.height)
        {
            NoteFramesDisplayed(1, 0);
            return false;
        }

        const u32 real_index = swap_chain->GetCurrentImageIndex();
        VkImage real_image = swap_chain->GetCurrentTexture()->GetImage();

        const bool have_previous = s_frame_index > 0;
        AhbImage& target = s_frame[s_frame_index % 2];

        const VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
        vkResetCommandBuffer(s_pre_copy_cmd, 0);
        if (vkBeginCommandBuffer(s_pre_copy_cmd, &begin) != VK_SUCCESS)
            return false;
        RecordCopy(s_pre_copy_cmd, real_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, target.image,
            VK_IMAGE_LAYOUT_GENERAL, s_extent);
        if (vkEndCommandBuffer(s_pre_copy_cmd) != VK_SUCCESS)
            return false;

        if (!SubmitOneShot(s_pre_copy_cmd, render_finished, s_pre_copy_sem))
            return false;

        s_frame_index++;

        if (!have_previous)
        {
            const VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &s_pre_copy_sem, 1,
                swap_chain->GetSwapChainPtr(), &real_index, nullptr};
            swap_chain->ResetImageAcquireResult();
            vkQueuePresentKHR(present_queue, &present);
            NoteFramesDisplayed(1, 0);
            return true;
        }

        vkQueueWaitIdle(s_queue);
        const bool generated = (s_backend.present(s_context_id) == 0);
        if (!generated)
        {
            Console.ErrorFmt("@@ANDROID_LSFG@@ generation failed: {}", BackendError());
        }
        else
            s_backend.wait_idle();

        u32 presented_generated = 0;
        if (generated)
        {
            for (u32 i = 0; i < s_multiplier - 1; i++)
            {
                u32 image_index = 0;
                static constexpr u64 kGeneratedAcquireTimeoutNs = 50ull * 1000 * 1000;
                const VkResult acq = vkAcquireNextImageKHR(s_device, swap_chain->GetSwapChain(),
                    kGeneratedAcquireTimeoutNs, s_acquire_sems[i], VK_NULL_HANDLE, &image_index);
                if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
                {
                    break;
                }

                vkResetCommandBuffer(s_post_copy_cmds[i], 0);
                if (vkBeginCommandBuffer(s_post_copy_cmds[i], &begin) != VK_SUCCESS)
                    break;
                RecordCopy(s_post_copy_cmds[i], s_generated[i].image, VK_IMAGE_LAYOUT_GENERAL,
                    swap_chain->GetImage(image_index), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, s_extent);
                if (vkEndCommandBuffer(s_post_copy_cmds[i]) != VK_SUCCESS)
                    break;

                if (!SubmitOneShot(s_post_copy_cmds[i], s_acquire_sems[i], s_post_copy_sems[i]))
                    break;

                const VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1,
                    &s_post_copy_sems[i], 1, swap_chain->GetSwapChainPtr(), &image_index, nullptr};
                const VkResult pres = vkQueuePresentKHR(present_queue, &present);
                if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR)
                    break;
                presented_generated++;
            }
        }

        const VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &s_pre_copy_sem, 1,
            swap_chain->GetSwapChainPtr(), &real_index, nullptr};
        swap_chain->ResetImageAcquireResult();
        vkQueuePresentKHR(present_queue, &present);
        NoteFramesDisplayed(1, presented_generated);
        return true;
    }
} // namespace GSLsfg

#else // !LSFG_IMPLEMENTATION_AVAILABLE

// --- Stubs for when the implementation is not compiled ---
namespace GSLsfg
{
    bool Initialize(VKSwapChain*, u32) { return false; }
    void Shutdown() {}
    bool IsActive() { return false; }
    u32 GetMultiplier() { return 1; }
    bool PresentWithGeneration(VkQueue, VKSwapChain*, VkSemaphore, bool) { return false; }
} // namespace GSLsfg

#endif // LSFG_IMPLEMENTATION_AVAILABLE
