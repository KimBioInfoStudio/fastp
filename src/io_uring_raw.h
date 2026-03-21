#ifndef FASTP_IO_URING_RAW_H
#define FASTP_IO_URING_RAW_H

#ifdef __linux__

#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

// io_uring syscall numbers (x86_64 and aarch64)
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup    425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter    426
#endif

// opcodes
#define FASTP_IORING_OP_WRITE  23

// io_uring_enter flags
#define FASTP_IORING_ENTER_GETEVENTS (1U << 0)

// mmap offsets
#define FASTP_IORING_OFF_SQ_RING  0ULL
#define FASTP_IORING_OFF_CQ_RING  0x8000000ULL
#define FASTP_IORING_OFF_SQES     0x10000000ULL

struct fastp_io_sqring_offsets {
    uint32_t head, tail, ring_mask, ring_entries;
    uint32_t flags, dropped, array, resv1;
    uint64_t resv2;
};

struct fastp_io_cqring_offsets {
    uint32_t head, tail, ring_mask, ring_entries;
    uint32_t overflow, cqes, flags, resv1;
    uint64_t resv2;
};

struct fastp_io_uring_params {
    uint32_t sq_entries, cq_entries, flags;
    uint32_t sq_thread_cpu, sq_thread_idle, features, wq_fd;
    uint32_t resv[3];
    struct fastp_io_sqring_offsets sq_off;
    struct fastp_io_cqring_offsets cq_off;
};

struct fastp_io_uring_sqe {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t ioprio;
    int32_t  fd;
    uint64_t off;
    uint64_t addr;
    uint32_t len;
    uint32_t rw_flags;
    uint64_t user_data;
    uint16_t buf_index;
    uint16_t personality;
    int32_t  splice_fd_in;
    uint64_t __pad2[2];
};

struct fastp_io_uring_cqe {
    uint64_t user_data;
    int32_t  res;
    uint32_t flags;
};

// Minimal io_uring wrapper using raw syscalls.
// Only supports IORING_OP_WRITE for fastp's pwrite replacement.
class IoUringRaw {
public:
    IoUringRaw() : ring_fd_(-1), sq_ring_(NULL), cq_ring_(NULL), sqes_(NULL) {}
    ~IoUringRaw() { teardown(); }

    // Try to initialize. Returns true on success, false if kernel doesn't support it.
    bool setup(uint32_t depth) {
        struct fastp_io_uring_params p;
        memset(&p, 0, sizeof(p));

        ring_fd_ = syscall(__NR_io_uring_setup, depth, &p);
        if (ring_fd_ < 0)
            return false;

        sq_entries_ = p.sq_entries;

        // mmap SQ ring
        sq_ring_sz_ = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
        sq_ring_ = (char*)mmap(NULL, sq_ring_sz_, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_POPULATE, ring_fd_, FASTP_IORING_OFF_SQ_RING);
        if (sq_ring_ == MAP_FAILED) goto fail_ring;

        sq_head_   = (uint32_t*)(sq_ring_ + p.sq_off.head);
        sq_tail_   = (uint32_t*)(sq_ring_ + p.sq_off.tail);
        sq_mask_   = *(uint32_t*)(sq_ring_ + p.sq_off.ring_mask);
        sq_array_  = (uint32_t*)(sq_ring_ + p.sq_off.array);

        // mmap SQEs
        sqes_sz_ = p.sq_entries * sizeof(struct fastp_io_uring_sqe);
        sqes_ = (struct fastp_io_uring_sqe*)mmap(NULL, sqes_sz_, PROT_READ | PROT_WRITE,
                                                  MAP_SHARED | MAP_POPULATE, ring_fd_, FASTP_IORING_OFF_SQES);
        if (sqes_ == MAP_FAILED) goto fail_sqes;

        // mmap CQ ring
        cq_ring_sz_ = p.cq_off.cqes + p.cq_entries * sizeof(struct fastp_io_uring_cqe);
        cq_ring_ = (char*)mmap(NULL, cq_ring_sz_, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_POPULATE, ring_fd_, FASTP_IORING_OFF_CQ_RING);
        if (cq_ring_ == MAP_FAILED) goto fail_cq;

        cq_head_ = (uint32_t*)(cq_ring_ + p.cq_off.head);
        cq_tail_ = (uint32_t*)(cq_ring_ + p.cq_off.tail);
        cq_mask_ = *(uint32_t*)(cq_ring_ + p.cq_off.ring_mask);
        cqes_    = (struct fastp_io_uring_cqe*)(cq_ring_ + p.cq_off.cqes);

        return true;

    fail_cq:
        munmap(sqes_, sqes_sz_);
    fail_sqes:
        munmap(sq_ring_, sq_ring_sz_);
    fail_ring:
        close(ring_fd_);
        ring_fd_ = -1;
        return false;
    }

    void teardown() {
        if (ring_fd_ < 0) return;
        munmap(cq_ring_, cq_ring_sz_);
        munmap(sqes_, sqes_sz_);
        munmap(sq_ring_, sq_ring_sz_);
        close(ring_fd_);
        ring_fd_ = -1;
    }

    // Submit a single write. Returns 0 on success, -errno on failure.
    // user_data is returned in CQE for identification.
    // Caller must hold external mutex if multi-threaded.
    int submitWrite(int fd, const void* buf, size_t len, uint64_t offset, uint64_t user_data) {
        uint32_t tail = __atomic_load_n(sq_tail_, __ATOMIC_RELAXED);
        uint32_t head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
        if (tail - head >= sq_entries_)
            return -EBUSY;

        uint32_t idx = tail & sq_mask_;
        struct fastp_io_uring_sqe* sqe = &sqes_[idx];
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode    = FASTP_IORING_OP_WRITE;
        sqe->fd        = fd;
        sqe->addr      = (uint64_t)(uintptr_t)buf;
        sqe->len       = (uint32_t)len;
        sqe->off       = offset;
        sqe->user_data = user_data;

        sq_array_[idx] = idx;
        __atomic_store_n(sq_tail_, tail + 1, __ATOMIC_RELEASE);

        int ret = syscall(__NR_io_uring_enter, ring_fd_, 1, 0, 0, NULL, (size_t)0);
        return ret < 0 ? -errno : 0;
    }

    // Drain all completed CQEs. Calls fn(cqe) for each.
    // Caller must hold external mutex if multi-threaded.
    template<typename Fn>
    int drainCqes(Fn&& fn) {
        int count = 0;
        while (true) {
            uint32_t head = __atomic_load_n(cq_head_, __ATOMIC_ACQUIRE);
            uint32_t tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
            if (head == tail)
                break;
            fn(&cqes_[head & cq_mask_]);
            __atomic_store_n(cq_head_, head + 1, __ATOMIC_RELEASE);
            count++;
        }
        return count;
    }

    // Block until at least one CQE is available.
    int waitCqe() {
        return syscall(__NR_io_uring_enter, ring_fd_, 0, 1,
                       FASTP_IORING_ENTER_GETEVENTS, NULL, (size_t)0);
    }

    bool isValid() const { return ring_fd_ >= 0; }

private:
    int ring_fd_;
    char* sq_ring_;
    char* cq_ring_;
    struct fastp_io_uring_sqe* sqes_;

    uint32_t* sq_head_;
    uint32_t* sq_tail_;
    uint32_t* sq_array_;
    uint32_t* cq_head_;
    uint32_t* cq_tail_;
    struct fastp_io_uring_cqe* cqes_;

    uint32_t sq_mask_;
    uint32_t cq_mask_;
    uint32_t sq_entries_;

    size_t sq_ring_sz_;
    size_t sqes_sz_;
    size_t cq_ring_sz_;
};

#endif // __linux__
#endif // FASTP_IO_URING_RAW_H
