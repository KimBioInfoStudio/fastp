#ifndef TRACE_PROFILER_H
#define TRACE_PROFILER_H

#include <stdint.h>
#include <string>
#include <vector>

namespace trace {

void init(bool cliEnabled, const std::string& cliOutputPath);
void flush();
bool enabled();
const std::string& outputPath();
uint64_t nowUs();

class TaskBreakdown {
public:
    TaskBreakdown(const std::string& taskName, const std::string& category);
    ~TaskBreakdown();

    void addGap(const std::string& gapName, uint64_t durUs);

private:
    std::string mTaskName;
    std::string mCategory;
    uint64_t mStartUs;
    std::vector<std::pair<std::string, uint64_t> > mGaps;
};

} // namespace trace

#endif
