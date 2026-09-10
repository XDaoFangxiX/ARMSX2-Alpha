// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "common/Timer.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <functional>
#include <queue>
#include <algorithm>

//==============================================================================
// Configuration Constants
//==============================================================================
namespace GSShaderCompileConfig
{
    // Optimized timing parameters
    inline constexpr u64 RECENT_COMPILE_HOLD_NS = 500'000'000ULL;        // 0.5 seconds
    inline constexpr u64 CACHE_CLEANUP_INTERVAL_NS = 30'000'000'000ULL;  // 30 seconds
    inline constexpr u32 MAX_CACHE_SIZE = 1024;                          // Maximum shader cache size
    
    // Performance tuning
    inline constexpr bool ENABLE_PARALLEL_COMPILE = true;
    inline constexpr bool ENABLE_SHADER_CACHE = true;
    inline constexpr bool ENABLE_PRECOMPILE = true;
    inline constexpr u32 COMPILE_TIMEOUT_MS = 5000;
    
    // Fade settings
    inline constexpr float FADE_POWER = 2.0f;  // Quadratic fade for smoother transition
}

//==============================================================================
// Enhanced Atomic Data Structure with Cache Line Alignment
//==============================================================================
struct alignas(64) ShaderCompileStats
{
    std::atomic<u32> count{0};
    std::atomic<u64> total_time_ns{0};
    std::atomic<u64> last_compile_time{0};
    std::atomic<u64> total_shaders_compiled{0};
    std::atomic<u64> cache_hits{0};
    std::atomic<u64> cache_misses{0};
    
    // Prevent false sharing
    char padding[64 - (sizeof(std::atomic<u32>) + 
                       sizeof(std::atomic<u64>) * 4)];
};

//==============================================================================
// Shader Cache System
//==============================================================================
class ShaderBinaryCache
{
private:
    struct CacheEntry
    {
        std::vector<u8> data;
        u64 last_access_time;
        u32 hash;
    };
    
    std::unordered_map<std::string, CacheEntry> m_cache;
    std::mutex m_mutex;
    u64 m_total_size = 0;
    u64 m_max_size = 100 * 1024 * 1024; // 100 MB limit
    
public:
    static ShaderBinaryCache& GetInstance()
    {
        static ShaderBinaryCache instance;
        return instance;
    }
    
    bool Get(const std::string& key, std::vector<u8>& out_data)
    {
        if (!GSShaderCompileConfig::ENABLE_SHADER_CACHE)
            return false;
            
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_cache.find(key);
        if (it != m_cache.end())
        {
            it->second.last_access_time = Common::Timer::GetCurrentValue();
            out_data = it->second.data;
            return true;
        }
        return false;
    }
    
    void Put(const std::string& key, const std::vector<u8>& data)
    {
        if (!GSShaderCompileConfig::ENABLE_SHADER_CACHE)
            return;
            
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Evict old entries if cache is full
        if (m_cache.size() >= GSShaderCompileConfig::MAX_CACHE_SIZE)
        {
            EvictOldest();
        }
        
        CacheEntry entry;
        entry.data = data;
        entry.last_access_time = Common::Timer::GetCurrentValue();
        entry.hash = std::hash<std::string>{}(key);
        
        m_cache[key] = std::move(entry);
        m_total_size += data.size();
    }
    
    void Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache.clear();
        m_total_size = 0;
    }
    
    size_t Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cache.size();
    }
    
private:
    void EvictOldest()
    {
        if (m_cache.empty())
            return;
            
        auto oldest = std::min_element(
            m_cache.begin(), m_cache.end(),
            [](const auto& a, const auto& b) {
                return a.second.last_access_time < b.second.last_access_time;
            }
        );
        
        if (oldest != m_cache.end())
        {
            m_total_size -= oldest->second.data.size();
            m_cache.erase(oldest);
        }
    }
};

//==============================================================================
// Parallel Shader Compiler
//==============================================================================
struct ShaderCompileTask
{
    std::string shader_source;
    std::string shader_key;
    std::function<void(const std::vector<u8>&)> callback;
    u64 start_time;
};

class ParallelShaderCompiler
{
private:
    std::vector<std::thread> m_workers;
    std::queue<ShaderCompileTask> m_task_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_stop{false};
    std::atomic<u32> m_active_tasks{0};
    
public:
    ParallelShaderCompiler()
    {
        if (GSShaderCompileConfig::ENABLE_PARALLEL_COMPILE)
        {
            u32 num_threads = std::max(1u, std::thread::hardware_concurrency() / 2);
            num_threads = std::min(num_threads, 4u); // Limit to 4 threads max
            
            for (u32 i = 0; i < num_threads; ++i)
            {
                m_workers.emplace_back(&ParallelShaderCompiler::WorkerLoop, this);
            }
        }
    }
    
    ~ParallelShaderCompiler()
    {
        m_stop = true;
        m_cv.notify_all();
        for (auto& worker : m_workers)
        {
            if (worker.joinable())
                worker.join();
        }
    }
    
    void SubmitTask(ShaderCompileTask&& task)
    {
        task.start_time = Common::Timer::GetCurrentValue();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_task_queue.push(std::move(task));
            m_active_tasks.fetch_add(1);
        }
        m_cv.notify_one();
    }
    
    u32 GetActiveTaskCount() const
    {
        return m_active_tasks.load(std::memory_order_acquire);
    }
    
private:
    void WorkerLoop()
    {
        while (!m_stop)
        {
            ShaderCompileTask task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] {
                    return !m_task_queue.empty() || m_stop;
                });
                
                if (m_stop && m_task_queue.empty())
                    break;
                    
                task = std::move(m_task_queue.front());
                m_task_queue.pop();
            }
            
            // Simulate shader compilation
            // In real implementation, this would call the actual shader compiler
            std::vector<u8> compiled_data = CompileShader(task.shader_source);
            
            // Cache the result
            ShaderBinaryCache::GetInstance().Put(task.shader_key, compiled_data);
            
            // Callback with result
            if (task.callback)
            {
                task.callback(compiled_data);
            }
            
            m_active_tasks.fetch_sub(1);
        }
    }
    
    std::vector<u8> CompileShader(const std::string& source)
    {
        // This is a placeholder - actual shader compilation would go here
        std::vector<u8> result;
        result.reserve(source.size());
        std::copy(source.begin(), source.end(), std::back_inserter(result));
        return result;
    }
};

//==============================================================================
// Main Shader Compile Indicator with Optimizations
//==============================================================================
namespace GSShaderCompileIndicator
{
    // Statistics storage with proper alignment
    static ShaderCompileStats s_stats;
    
    // Global compiler instance
    static ParallelShaderCompiler s_compiler;
    
    //==========================================================================
    // Core Functions
    //==========================================================================
    
    inline u64 GetRecentCompileHold()
    {
        static const u64 hold = static_cast<u64>(
            Common::Timer::ConvertNanosecondsToValue(
                static_cast<double>(GSShaderCompileConfig::RECENT_COMPILE_HOLD_NS)));
        return hold;
    }
    
    inline void OnCompileDone(u64 duration_ns, u64 start_time)
    {
        const u64 now = Common::Timer::GetCurrentValue();
        u64 last = s_stats.last_compile_time.load(std::memory_order_acquire);
        
        // Reset counters if compilation activity has paused
        if (last != 0 && start_time > last && 
            (start_time - last) >= GetRecentCompileHold())
        {
            s_stats.count.store(0, std::memory_order_relaxed);
            s_stats.total_time_ns.store(0, std::memory_order_relaxed);
        }
        
        // Update statistics atomically
        s_stats.count.fetch_add(1, std::memory_order_relaxed);
        s_stats.total_time_ns.fetch_add(duration_ns, std::memory_order_relaxed);
        s_stats.total_shaders_compiled.fetch_add(1, std::memory_order_relaxed);
        s_stats.last_compile_time.store(now, std::memory_order_release);
    }
    
    inline u32 GetCount()
    {
        return s_stats.count.load(std::memory_order_acquire);
    }
    
    inline u32 GetTimeMs()
    {
        const u64 time_ns = s_stats.total_time_ns.load(std::memory_order_acquire);
        const u32 ms = static_cast<u32>(time_ns / 1'000'000);
        return (ms > 0) ? ms : (GetCount() > 0 ? 1u : 0u);
    }
    
    inline u64 GetTotalShadersCompiled()
    {
        return s_stats.total_shaders_compiled.load(std::memory_order_acquire);
    }
    
    inline bool IsVisible()
    {
        if (GetCount() == 0)
            return false;
            
        const u64 last = s_stats.last_compile_time.load(std::memory_order_acquire);
        if (last == 0)
            return false;
            
        return (Common::Timer::GetCurrentValue() - last) < GetRecentCompileHold();
    }
    
    inline float GetFadeAlpha()
    {
        const u64 last = s_stats.last_compile_time.load(std::memory_order_acquire);
        if (last == 0)
            return 0.0f;
            
        const u64 now = Common::Timer::GetCurrentValue();
        if (now <= last)
            return 1.0f;
            
        const u64 hold = GetRecentCompileHold();
        const u64 elapsed = now - last;
        if (elapsed >= hold)
            return 0.0f;
        
        // Advanced fading with configurable power
        const float t = static_cast<float>(elapsed) / static_cast<float>(hold);
        return 1.0f - std::pow(t, GSShaderCompileConfig::FADE_POWER);
    }
    
    //==========================================================================
    // Cache Statistics
    //==========================================================================
    
    inline void RecordCacheHit()
    {
        s_stats.cache_hits.fetch_add(1, std::memory_order_relaxed);
    }
    
    inline void RecordCacheMiss()
    {
        s_stats.cache_misses.fetch_add(1, std::memory_order_relaxed);
    }
    
    inline double GetCacheHitRate()
    {
        const u64 hits = s_stats.cache_hits.load(std::memory_order_acquire);
        const u64 misses = s_stats.cache_misses.load(std::memory_order_acquire);
        const u64 total = hits + misses;
        return (total > 0) ? (static_cast<double>(hits) / total * 100.0) : 0.0;
    }
    
    //==========================================================================
    // Optimized Shader Compilation
    //==========================================================================
    
    struct CompileResult
    {
        std::vector<u8> data;
        u64 duration_ns;
        bool from_cache;
    };
    
    inline CompileResult CompileShaderWithCache(const std::string& key, const std::string& source)
    {
        CompileResult result;
        result.from_cache = false;
        
        // Try cache first
        std::vector<u8> cached_data;
        if (ShaderBinaryCache::GetInstance().Get(key, cached_data))
        {
            result.data = std::move(cached_data);
            result.duration_ns = 0;
            result.from_cache = true;
            RecordCacheHit();
            return result;
        }
        
        RecordCacheMiss();
        
        // Actual compilation would happen here
        // For now, simulate compilation
        const u64 start_time = Common::Timer::GetCurrentValue();
        
        // Simulate work - this is where real shader compilation would happen
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        // Create compiled data
        std::vector<u8> compiled_data;
        compiled_data.reserve(source.size() * 2);
        
        // This is a placeholder for actual compilation
        for (char c : source)
        {
            compiled_data.push_back(static_cast<u8>(c));
            compiled_data.push_back(static_cast<u8>(c ^ 0xFF));
        }
        
        result.data = std::move(compiled_data);
        result.duration_ns = Common::Timer::GetCurrentValue() - start_time;
        
        // Cache the result
        ShaderBinaryCache::GetInstance().Put(key, result.data);
        
        // Update statistics
        OnCompileDone(result.duration_ns, start_time);
        
        return result;
    }
    
    //==========================================================================
    // Async Compilation
    //==========================================================================
    
    inline void CompileShaderAsync(const std::string& key, const std::string& source,
                                   std::function<void(const std::vector<u8>&)> callback = nullptr)
    {
        ShaderCompileTask task;
        task.shader_source = source;
        task.shader_key = key;
        task.callback = callback;
        task.start_time = Common::Timer::GetCurrentValue();
        
        s_compiler.SubmitTask(std::move(task));
    }
    
    //==========================================================================
    // Performance Metrics
    //==========================================================================
    
    struct PerformanceMetrics
    {
        u32 current_shaders;
        u32 compile_time_ms;
        u64 total_shaders;
        double cache_hit_rate;
        float fade_alpha;
        bool visible;
        u32 active_tasks;
    };
    
    inline PerformanceMetrics GetPerformanceMetrics()
    {
        PerformanceMetrics metrics;
        metrics.current_shaders = GetCount();
        metrics.compile_time_ms = GetTimeMs();
        metrics.total_shaders = GetTotalShadersCompiled();
        metrics.cache_hit_rate = GetCacheHitRate();
        metrics.fade_alpha = GetFadeAlpha();
        metrics.visible = IsVisible();
        metrics.active_tasks = s_compiler.GetActiveTaskCount();
        return metrics;
    }
    
    //==========================================================================
    // Timer Class with RAII
    //==========================================================================
    
    struct CompileTimer
    {
        Common::Timer timer;
        bool completed = false;
        
        CompileTimer() = default;
        
        ~CompileTimer()
        {
            if (!completed)
            {
                const u64 duration = static_cast<u64>(timer.GetTimeNanoseconds());
                const u64 start = timer.GetStartValue();
                OnCompileDone(duration, start);
                completed = true;
            }
        }
        
        // Manual completion for early reporting
        void Complete()
        {
            if (!completed)
            {
                const u64 duration = static_cast<u64>(timer.GetTimeNanoseconds());
                const u64 start = timer.GetStartValue();
                OnCompileDone(duration, start);
                completed = true;
            }
        }
        
        // Prevent copying
        CompileTimer(const CompileTimer&) = delete;
        CompileTimer& operator=(const CompileTimer&) = delete;
        
        // Allow moving
        CompileTimer(CompileTimer&& other) noexcept
            : timer(std::move(other.timer))
            , completed(other.completed)
        {
            other.completed = true;
        }
        
        CompileTimer& operator=(CompileTimer&& other) noexcept
        {
            if (this != &other)
            {
                timer = std::move(other.timer);
                completed = other.completed;
                other.completed = true;
            }
            return *this;
        }
    };
    
    //==========================================================================
    // Utility Functions
    //==========================================================================
    
    inline void ResetStatistics()
    {
        s_stats.count.store(0, std::memory_order_relaxed);
        s_stats.total_time_ns.store(0, std::memory_order_relaxed);
        s_stats.total_shaders_compiled.store(0, std::memory_order_relaxed);
        s_stats.cache_hits.store(0, std::memory_order_relaxed);
        s_stats.cache_misses.store(0, std::memory_order_relaxed);
    }
    
    inline void ClearCache()
    {
        ShaderBinaryCache::GetInstance().Clear();
    }
    
    inline size_t GetCacheSize()
    {
        return ShaderBinaryCache::GetInstance().Size();
    }
}

//==============================================================================
// Usage Example
//==============================================================================

/*
Example usage:

```cpp
// Initialize and precompile common shaders
void InitializeShaderSystem()
{
    // Precompile frequently used shaders
    std::vector<std::string> common_shaders = {
        "vertex_shader_standard",
        "pixel_shader_standard",
        "compute_shader_basic"
    };
    
    for (const auto& key : common_shaders)
    {
        // This will use cache if available
        auto result = GSShaderCompileIndicator::CompileShaderWithCache(key, "shader_source");
    }
}

// Compile shader asynchronously
void CompileShaderAsyncExample()
{
    GSShaderCompileIndicator::CompileShaderAsync(
        "my_shader_key",
        "shader_source_code",
        [](const std::vector<u8>& compiled_data) {
            // Handle compiled shader data
        }
    );
}

// Monitor compilation progress
void UpdateShaderIndicator()
{
    auto metrics = GSShaderCompileIndicator::GetPerformanceMetrics();
    
    if (metrics.visible)
    {
        float alpha = metrics.fade_alpha;
        u32 count = metrics.current_shaders;
        u32 time_ms = metrics.compile_time_ms;
        double cache_rate = metrics.cache_hit_rate;
        
        // Update UI with compilation status
        // Shader compilation indicator with fade effect
    }
}

// Use RAII timer for automatic tracking
void CompileShaderWithTracking()
{
    GSShaderCompileIndicator::CompileTimer timer;
    
    // Perform shader compilation
    CompileShader();
    
    // Timer automatically updates statistics on destruction
}
