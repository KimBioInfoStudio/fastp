// End-to-end benchmarks for fastplong: 100K ONT long reads (gz + ungz).
// Test data is generated at runtime with a fixed random seed so that
// every run processes the same sequences.
//
// ONT characteristics:
//   - Read lengths: log-normal distribution, median ~5 kb, range 200 bp – 100 kb
//   - Quality scores: mean ~Q12 (typical ONT), with occasional low-quality dips
//   - 0.5% N-base rate
//   - ONT adapter sequences on ~70% of reads

#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// ONT FASTQ data generator (fixed-seed, deterministic)
// ---------------------------------------------------------------------------

static constexpr int      NUM_READS = 100'000;
static constexpr uint32_t SEED      = 42;
static const char BASES[] = "ACGT";

// Real ONT adapter sequences (Oxford Nanopore)
static constexpr const char START_ADAPTER[] = "AATGTACTTCGTTCAGTTACGTATTGCT"; // 28 bp
static constexpr const char END_ADAPTER[]   = "GCAATACGTAACTGAACGAAGT";       // 22 bp
static constexpr int SA_LEN = 28;
static constexpr int EA_LEN = 22;

static int generateReadLength(std::mt19937& rng) {
    // Log-normal: median ~5000 bp, long tail up to ~100 kbp
    std::lognormal_distribution<double> dist(std::log(5000.0), 0.7);
    int len = static_cast<int>(dist(rng));
    return std::max(200, std::min(100000, len));
}

static void writeOntFastq(const fs::path& path, int numReads,
                          std::mt19937& rng) {
    std::uniform_int_distribution<int>  baseDist(0, 3);
    std::normal_distribution<double>    qualBaseDist(12.0, 3.0);
    std::normal_distribution<double>    qualNoiseDist(0.0, 4.0);
    std::normal_distribution<double>    qualLowDist(5.0, 2.0);
    std::uniform_real_distribution<double> unitDist(0.0, 1.0);
    std::uniform_int_distribution<int>  chDist(1, 512);

    std::ofstream out(path, std::ios::binary);
    constexpr size_t FLUSH = 128 << 20; // 128 MB

    std::string buf;
    buf.reserve(FLUSH + (2 << 20));

    std::string seq, qual;
    seq.reserve(110000);
    qual.reserve(110000);

    for (int i = 0; i < numReads; i++) {
        int len = generateReadLength(rng);

        // Header (ONT-style)
        buf += "@ont_bench_";
        buf += std::to_string(i);
        buf += " ch=";
        buf += std::to_string(chDist(rng));
        buf += '\n';

        // Adapter placement: 10% both, 30% start-only, 30% end-only, 30% none
        seq.clear();
        double r = unitDist(rng);
        bool front = (r < 0.40);
        bool back  = (r < 0.10 || (r >= 0.40 && r < 0.70));

        if (front) seq.append(START_ADAPTER, SA_LEN);

        int coreLen = len;
        if (front) coreLen -= SA_LEN;
        if (back)  coreLen -= EA_LEN;
        coreLen = std::max(200, coreLen);

        size_t pos = seq.size();
        seq.resize(pos + coreLen);
        for (int j = 0; j < coreLen; j++)
            seq[pos + j] = (unitDist(rng) < 0.005)
                               ? 'N'
                               : BASES[baseDist(rng)];

        if (back) seq.append(END_ADAPTER, EA_LEN);

        buf.append(seq);
        buf += "\n+\n";

        // Quality (ONT-like: per-read baseline ~Q12, occasional dips)
        int actualLen = static_cast<int>(seq.size());
        double baseQ = qualBaseDist(rng);
        qual.resize(actualLen);
        for (int j = 0; j < actualLen; j++) {
            int q;
            if (unitDist(rng) < 0.02)
                q = static_cast<int>(qualLowDist(rng));
            else
                q = static_cast<int>(baseQ + qualNoiseDist(rng));
            qual[j] = static_cast<char>(std::clamp(q, 0, 40) + 33);
        }
        buf.append(qual);
        buf += '\n';

        if (buf.size() >= FLUSH) {
            out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            buf.clear();
        }
    }
    if (!buf.empty())
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
}

// ---------------------------------------------------------------------------
// Fixture: generates ONT data once before the suite
// ---------------------------------------------------------------------------

class FastplongBench : public ::testing::Test {
protected:
    static fs::path dir;

    static void SetUpTestSuite() {
        dir = fs::temp_directory_path() / "fastplong_bench";
        fs::create_directories(dir);

        std::mt19937 rng(SEED);
        writeOntFastq(dir / "ONT_100K.fq", NUM_READS, rng);

        // Compress to .gz (keep original)
        std::string cmd = "gzip -kf " + (dir / "ONT_100K.fq").string();
        ::system(cmd.c_str());
    }

    static void TearDownTestSuite() {
        fs::remove_all(dir);
    }

    // Run fastplong with given args, return wall-clock milliseconds.
    double runFastplong(const std::string& args) {
        std::string cmd = "./bin/fastplong " + args + " 2>/dev/null";
        auto t0 = std::chrono::high_resolution_clock::now();
        int ret  = ::system(cmd.c_str());
        auto t1  = std::chrono::high_resolution_clock::now();
        EXPECT_EQ(WEXITSTATUS(ret), 0);
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    }

    std::string se(const std::string& file) {
        return "-i " + (dir / file).string() +
               " -o /dev/null -j /dev/null -h /dev/null";
    }
};

fs::path FastplongBench::dir;

// ---------------------------------------------------------------------------
// JSON result collector (customSmallerIsBetter for github-action-benchmark)
// ---------------------------------------------------------------------------

static std::vector<std::pair<std::string, double>> benchResults;

static void writeBenchJson(const char* path) {
    std::ofstream out(path);
    out << "[\n";
    for (size_t i = 0; i < benchResults.size(); i++) {
        out << "  {\"name\": \"" << benchResults[i].first
            << "\", \"unit\": \"ms\", \"value\": " << benchResults[i].second << "}";
        if (i + 1 < benchResults.size()) out << ",";
        out << "\n";
    }
    out << "]\n";
}

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

TEST_F(FastplongBench, ONT_100K_ungz) {
    double ms = runFastplong(se("ONT_100K.fq"));
    printf("  fastplong ONT 100K ungz:  %.0f ms\n", ms);
    benchResults.emplace_back("fastplong ONT 100K ungz", ms);
}

TEST_F(FastplongBench, ONT_100K_gz) {
    double ms = runFastplong(se("ONT_100K.fq.gz"));
    printf("  fastplong ONT 100K gz:    %.0f ms\n", ms);
    benchResults.emplace_back("fastplong ONT 100K gz", ms);
}

// Write JSON after all benchmarks complete
TEST_F(FastplongBench, ZZ_WriteResults) {
    const char* path = std::getenv("BENCH_JSON_OUTPUT");
    if (path) writeBenchJson(path);
}
