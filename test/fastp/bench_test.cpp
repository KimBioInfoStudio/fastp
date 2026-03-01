// End-to-end benchmarks for fastp: 1M SE150 and 1M PE150 (gz + ungz).
// Test data is generated at runtime with a fixed random seed so that
// every run processes the same sequences.

#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// FASTQ data generator (fixed-seed, deterministic)
// ---------------------------------------------------------------------------

static constexpr int NUM_READS = 1'000'000;
static constexpr int READ_LEN  = 150;
static constexpr uint32_t SEED = 42;
static const char BASES[] = "ACGT";

static void writeFastq(const fs::path& path, int numReads,
                       std::mt19937& rng, int readNum) {
    // readNum: 1 for R1/SE, 2 for R2
    std::uniform_int_distribution<int> baseDist(0, 3);
    std::uniform_int_distribution<int> qualHigh(30, 40);
    std::uniform_int_distribution<int> qualLow(20, 35);

    // ~350 bytes per read → preallocate 350 MB buffer is too much.
    // Write in 100K-read batches instead.
    constexpr int BATCH = 100'000;
    std::ofstream out(path, std::ios::binary);

    std::string seq(READ_LEN, 'N');
    std::string qual(READ_LEN, '!');
    std::string buf;
    buf.reserve(BATCH * 360);

    for (int i = 0; i < numReads; i++) {
        // Header
        buf += "@SIM:BENCH:1:1101:";
        buf += std::to_string(i);
        buf += ":0 ";
        buf += std::to_string(readNum);
        buf += ":N:0:ATCG\n";
        // Sequence
        for (int j = 0; j < READ_LEN; j++)
            seq[j] = BASES[baseDist(rng)];
        buf += seq;
        buf += "\n+\n";
        // Quality (high in middle, lower at ends — realistic-ish)
        for (int j = 0; j < READ_LEN; j++) {
            int q = (j < 5 || j >= READ_LEN - 10) ? qualLow(rng) : qualHigh(rng);
            qual[j] = static_cast<char>(q + 33);
        }
        buf += qual;
        buf += '\n';

        if ((i + 1) % BATCH == 0 || i == numReads - 1) {
            out.write(buf.data(), buf.size());
            buf.clear();
        }
    }
}

// ---------------------------------------------------------------------------
// Fixture: generates all data files once before the suite
// ---------------------------------------------------------------------------

class FastpBench : public ::testing::Test {
protected:
    static fs::path dir;

    static void SetUpTestSuite() {
        dir = fs::temp_directory_path() / "fastp_bench";
        fs::create_directories(dir);

        // Use separate RNG instances seeded deterministically so that
        // SE and PE data are independent but reproducible.
        std::mt19937 rngSE(SEED);
        writeFastq(dir / "SE_1M.fq", NUM_READS, rngSE, 1);

        std::mt19937 rngPE(SEED + 1);
        writeFastq(dir / "PE_R1_1M.fq", NUM_READS, rngPE, 1);
        writeFastq(dir / "PE_R2_1M.fq", NUM_READS, rngPE, 2);

        // Compress to .gz (keep originals)
        for (auto& name : {"SE_1M.fq", "PE_R1_1M.fq", "PE_R2_1M.fq"}) {
            std::string cmd = "gzip -kf " + (dir / name).string();
            ::system(cmd.c_str());
        }
    }

    static void TearDownTestSuite() {
        fs::remove_all(dir);
    }

    // Run fastp with given args, return wall-clock milliseconds.
    double runFastp(const std::string& args) {
        std::string cmd = "./bin/fastp " + args + " 2>/dev/null";
        auto t0 = std::chrono::high_resolution_clock::now();
        int ret = ::system(cmd.c_str());
        auto t1 = std::chrono::high_resolution_clock::now();
        EXPECT_EQ(WEXITSTATUS(ret), 0);
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    }

    std::string se(const std::string& file) {
        return "-i " + (dir / file).string() +
               " -o /dev/null -j /dev/null -h /dev/null";
    }

    std::string pe(const std::string& r1, const std::string& r2) {
        // fastp rejects -o and -O pointing to the same file,
        // so write R2 output to a second sink.
        std::string out2 = (dir / "out_R2.fq").string();
        return "-i " + (dir / r1).string() +
               " -I " + (dir / r2).string() +
               " -o /dev/null -O " + out2 +
               " -j /dev/null -h /dev/null";
    }
};

fs::path FastpBench::dir;

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

TEST_F(FastpBench, SE150_1M_ungz) {
    double ms = runFastp(se("SE_1M.fq"));
    printf("  fastp SE150 1M ungz:  %.0f ms\n", ms);
}

TEST_F(FastpBench, SE150_1M_gz) {
    double ms = runFastp(se("SE_1M.fq.gz"));
    printf("  fastp SE150 1M gz:    %.0f ms\n", ms);
}

TEST_F(FastpBench, PE150_1M_ungz) {
    double ms = runFastp(pe("PE_R1_1M.fq", "PE_R2_1M.fq"));
    printf("  fastp PE150 1M ungz:  %.0f ms\n", ms);
}

TEST_F(FastpBench, PE150_1M_gz) {
    double ms = runFastp(pe("PE_R1_1M.fq.gz", "PE_R2_1M.fq.gz"));
    printf("  fastp PE150 1M gz:    %.0f ms\n", ms);
}
