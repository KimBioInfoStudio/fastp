#include "processor.h"
#include "peprocessor.h"
#include "seprocessor.h"
#include "trace_profiler.h"

Processor::Processor(Options* opt){
    mOptions = opt;
}


Processor::~Processor(){
}

bool Processor::process() {
    trace::TaskBreakdown task("pipeline.process", "pipeline");
    if(mOptions->isPaired()) {
        PairEndProcessor p(mOptions);
        p.process();
    } else {
        SingleEndProcessor p(mOptions);
        p.process();
    }

    return true;
}
