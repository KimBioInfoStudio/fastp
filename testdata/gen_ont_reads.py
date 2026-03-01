#!/usr/bin/env python3
"""Generate dummy ONT (Oxford Nanopore) FASTQ reads for benchmarking.

Produces 10,000 reads with realistic ONT characteristics:
- Read lengths: log-normal distribution, median ~5kb, range ~500bp-50kb
- Quality scores: mean ~Q10-Q15 (typical ONT), with some low-quality regions
- Random ATCG sequence with occasional N bases
- Adapter sequences at start/end of some reads
"""

import random
import gzip
import math
import os

SEED = 42
NUM_READS = 10_000
OUTPUT_DIR = os.path.dirname(os.path.abspath(__file__))

# ONT adapter sequences (real ones from Oxford Nanopore)
START_ADAPTER = "AATGTACTTCGTTCAGTTACGTATTGCT"
END_ADAPTER = "GCAATACGTAACTGAACGAAGT"

BASES = "ACGT"


def random_quality(length: int) -> str:
    """Generate ONT-like quality string (phred+33).

    ONT typical range: Q5-Q20, with occasional dips.
    """
    quals = []
    # Start with a baseline quality that varies per read
    base_qual = random.gauss(12, 3)  # mean Q12

    for i in range(length):
        # Occasional low-quality regions (simulating systematic errors)
        if random.random() < 0.02:
            q = max(0, min(40, int(random.gauss(5, 2))))
        else:
            q = max(0, min(40, int(random.gauss(base_qual, 4))))
        quals.append(chr(q + 33))
    return "".join(quals)


def random_sequence(length: int) -> str:
    """Generate random DNA sequence with occasional N bases."""
    seq = []
    for _ in range(length):
        if random.random() < 0.005:  # 0.5% N rate
            seq.append("N")
        else:
            seq.append(random.choice(BASES))
    return "".join(seq)


def generate_read_length() -> int:
    """Log-normal distribution mimicking ONT read lengths."""
    # median ~5000, with long tail up to ~50000
    length = int(random.lognormvariate(math.log(5000), 0.7))
    return max(200, min(100000, length))


def write_fastq(filepath: str, reads: list, compress: bool = False):
    """Write reads to FASTQ file, optionally gzipped."""
    opener = gzip.open if compress else open
    mode = "wt" if compress else "w"

    with opener(filepath, mode) as f:
        for read_id, seq, qual in reads:
            f.write(f"@{read_id}\n")
            f.write(f"{seq}\n")
            f.write("+\n")
            f.write(f"{qual}\n")


def main():
    random.seed(SEED)

    reads = []
    total_bases = 0

    for i in range(NUM_READS):
        length = generate_read_length()

        # Some reads have adapters (30% start, 30% end, 10% both)
        seq = ""
        r = random.random()
        if r < 0.10:
            # Both adapters
            core_len = max(100, length - len(START_ADAPTER) - len(END_ADAPTER))
            seq = START_ADAPTER + random_sequence(core_len) + END_ADAPTER
        elif r < 0.40:
            # Start adapter only
            core_len = max(100, length - len(START_ADAPTER))
            seq = START_ADAPTER + random_sequence(core_len)
        elif r < 0.70:
            # End adapter only
            core_len = max(100, length - len(END_ADAPTER))
            seq = random_sequence(core_len) + END_ADAPTER
        else:
            seq = random_sequence(length)

        actual_len = len(seq)
        qual = random_quality(actual_len)

        read_id = (
            f"ont_read_{i:06d} "
            f"runid=dummy_run_001 "
            f"read={i} "
            f"ch={random.randint(1, 512)} "
            f"start_time=2024-01-01T00:00:{i % 60:02d}Z "
            f"flow_cell_id=FAK00001 "
            f"protocol_group_id=benchmark "
            f"sample_id=dummy_ont"
        )

        reads.append((read_id, seq, qual))
        total_bases += actual_len

    # Write uncompressed
    fq_path = os.path.join(OUTPUT_DIR, "ont_10k.fastq")
    write_fastq(fq_path, reads, compress=False)

    # Write gzipped
    gz_path = os.path.join(OUTPUT_DIR, "ont_10k.fastq.gz")
    write_fastq(gz_path, reads, compress=True)

    # Stats
    lengths = [len(seq) for _, seq, _ in reads]
    lengths.sort()
    print(f"Generated {NUM_READS} ONT reads")
    print(f"Total bases: {total_bases:,}")
    print(f"Mean length: {total_bases // NUM_READS:,}")
    print(f"Median length: {lengths[len(lengths)//2]:,}")
    print(f"Min length: {lengths[0]:,}")
    print(f"Max length: {lengths[-1]:,}")
    print(f"N50: {calculate_n50(lengths, total_bases):,}")
    print(f"Written: {fq_path} ({os.path.getsize(fq_path) / 1024 / 1024:.1f} MB)")
    print(f"Written: {gz_path} ({os.path.getsize(gz_path) / 1024 / 1024:.1f} MB)")


def calculate_n50(sorted_lengths: list, total_bases: int) -> int:
    cumsum = 0
    for l in reversed(sorted_lengths):
        cumsum += l
        if cumsum >= total_bases / 2:
            return l
    return sorted_lengths[-1]


if __name__ == "__main__":
    main()
