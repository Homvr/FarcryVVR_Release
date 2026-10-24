#pragma once

#include <Windows.h>
#include <chrono>
#include <iostream>
#include <string>

#if defined(DEBUG_PROFILING_ENABLED) && defined(_DEBUG)
#define SCOPE_PROFILER() ScopeProfiler scopeProfilerInstance(__FUNCTION__)
#else
#define SCOPE_PROFILER() // Macro does nothing when profiling is disabled
#endif

class ScopeProfiler {
public:
    inline ScopeProfiler(const std::string& id)
        : m_id(id), m_startTime(std::chrono::high_resolution_clock::now()), m_stopped(false) {
        m_indentLevel = s_indentLevel++;
        spdlog::info("[Profiler]{} {} {} started.", get_counters(), getIndent(), m_id);
    }

    inline ~ScopeProfiler() {
        if (!m_stopped) {
            stop();
        }
        s_indentLevel--;
    }

    inline void stop() {
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<float, std::milli>(endTime - m_startTime).count();
        spdlog::info("[Profiler]{} {} {} took {} millis.", get_counters(), getIndent(), m_id, duration);
        m_stopped = true;
    }

private:


    static std::string get_counters();

    static inline std::string getIndent() {
        return std::string(s_indentLevel * 2, ' '); // Adjust indentation as needed
    }
    std::string m_id;
    int m_indentLevel;
    std::chrono::high_resolution_clock::time_point m_startTime;
    bool m_stopped;
    inline static thread_local int s_indentLevel = 0;
};