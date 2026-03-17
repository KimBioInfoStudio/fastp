#include "trace_profiler.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <utility>

namespace trace {
namespace {
bool envTruthy(const char* v) {
    if (!v || v[0] == '\0')
        return false;
    return (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T');
}

struct Event {
    std::string name;
    std::string cat;
    uint64_t tsUs;
    uint64_t durUs;
    int pid;
    uint64_t tid;
};

class TraceState {
public:
    TraceState()
        : mEnabled(false), mOutputPath("fastp.trace.json") {}

    void init(bool cliEnabled, const std::string& cliOutputPath) {
        std::lock_guard<std::mutex> lk(mMu);
        const bool envEnabled = envTruthy(std::getenv("FASTP_TRACE"));
        mEnabled = cliEnabled || envEnabled;

        const char* envOut = std::getenv("FASTP_TRACE_FILE");
        if (!cliOutputPath.empty()) {
            mOutputPath = cliOutputPath;
        } else if (envOut && envOut[0] != '\0') {
            mOutputPath = envOut;
        } else {
            mOutputPath = "fastp.trace.json";
        }
    }

    bool enabled() const { return mEnabled; }

    const std::string& outputPath() const { return mOutputPath; }

    void addComplete(const std::string& name, const std::string& cat, uint64_t tsUs, uint64_t durUs) {
        if (!mEnabled)
            return;
        Event e;
        e.name = name;
        e.cat = cat;
        e.tsUs = tsUs;
        e.durUs = durUs;
        e.pid = (int)getpid();
        e.tid = tidNumber();
        std::lock_guard<std::mutex> lk(mMu);
        mEvents.push_back(e);
    }

    void flush() {
        if (!mEnabled)
            return;
        std::vector<Event> snapshot;
        {
            std::lock_guard<std::mutex> lk(mMu);
            snapshot.swap(mEvents);
        }

        std::ofstream ofs(mOutputPath.c_str(), std::ios::out | std::ios::trunc);
        if (!ofs.good())
            return;

        ofs << "{\"traceEvents\":[";
        for (size_t i = 0; i < snapshot.size(); i++) {
            if (i > 0)
                ofs << ",";
            const Event& e = snapshot[i];
            ofs << "{\"name\":\"" << escape(e.name)
                << "\",\"cat\":\"" << escape(e.cat)
                << "\",\"ph\":\"X\""
                << ",\"ts\":" << e.tsUs
                << ",\"dur\":" << e.durUs
                << ",\"pid\":" << e.pid
                << ",\"tid\":" << e.tid
                << "}";
        }
        ofs << "],\"displayTimeUnit\":\"ms\"}\n";
    }

private:
    static uint64_t tidNumber() {
        std::ostringstream oss;
        oss << std::this_thread::get_id();
        std::hash<std::string> h;
        return (uint64_t)h(oss.str());
    }

    static std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (size_t i = 0; i < s.size(); i++) {
            const char c = s[i];
            if (c == '\\' || c == '"') {
                out.push_back('\\');
                out.push_back(c);
            } else if (c == '\n') {
                out += "\\n";
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

private:
    bool mEnabled;
    std::string mOutputPath;
    std::vector<Event> mEvents;
    mutable std::mutex mMu;
};

TraceState& state() {
    static TraceState s;
    return s;
}

} // namespace

uint64_t nowUs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void init(bool cliEnabled, const std::string& cliOutputPath) {
    state().init(cliEnabled, cliOutputPath);
}

void flush() {
    state().flush();
}

bool enabled() {
    return state().enabled();
}

const std::string& outputPath() {
    return state().outputPath();
}

TaskBreakdown::TaskBreakdown(const std::string& taskName, const std::string& category)
    : mTaskName(taskName), mCategory(category), mStartUs(nowUs()) {
}

TaskBreakdown::~TaskBreakdown() {
    if (!enabled())
        return;

    const uint64_t endUs = nowUs();
    const uint64_t totalUs = (endUs > mStartUs ? endUs - mStartUs : 0);
    uint64_t gapUs = 0;
    for (size_t i = 0; i < mGaps.size(); i++)
        gapUs += mGaps[i].second;
    const uint64_t busyUs = (totalUs > gapUs ? totalUs - gapUs : 0);

    state().addComplete(mTaskName + ".total", mCategory, mStartUs, totalUs);
    state().addComplete(mTaskName + ".busy", mCategory, mStartUs, busyUs);

    uint64_t cursor = mStartUs;
    for (size_t i = 0; i < mGaps.size(); i++) {
        const std::string name = mTaskName + ".gap." + mGaps[i].first;
        state().addComplete(name, mCategory, cursor, mGaps[i].second);
        cursor += mGaps[i].second;
    }
}

void TaskBreakdown::addGap(const std::string& gapName, uint64_t durUs) {
    if (!enabled() || durUs == 0)
        return;
    for (size_t i = 0; i < mGaps.size(); i++) {
        if (mGaps[i].first == gapName) {
            mGaps[i].second += durUs;
            return;
        }
    }
    mGaps.push_back(std::make_pair(gapName, durUs));
}

} // namespace trace
