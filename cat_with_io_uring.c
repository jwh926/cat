#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <linux/fs.h>
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/ioctl.h>

#define QUEUE_DEPTH 1
#define FILE_BLOCK_SIZE 4096

/* something that i dont canonically have any knowledge of */
#define read_barrier() __asm__ __volatile__("" ::: "memory")
#define write_barrier() __asm__ __volatile__("" ::: "memory")

/* mimicing submission queue ring buffer */
struct app_io_sq_ring {
    unsigned *head;
    unsigned *tail;
    unsigned *ring_mask;
    unsigned *ring_entries;
    unsigned *flags;
    unsigned *array;
};
/* mimicing completion queue ring buffer */
struct app_io_cq_ring {
    unsigned *head;
    unsigned *tail;
    unsigned *ring_mask;
    unsigned *ring_entries;
    struct io_uring_cqe *cqes;
};
/* submitter */
struct submitter {
    int ring_fd;
    struct app_io_sq_ring sq_ring;
    struct io_uring_sqe *sqes;
    struct app_io_cq_ring cq_ring;
};
/* file data represented as iovecs */
struct file_info {
    off_t file_size;
    struct iovec iovecs[];
};
/* gotta tell the kernel setup io_urings and params */
int io_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return (int) syscall(__NR_io_uring_setup, entries, p);
}
/* submit sq to the kernel */
int io_uring_enter(int ring_fd, unsigned int to_submit, unsigned int min_complete, unsigned int flags)
{
    return (int) syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
}

/* i have no idea what i'm doing */
off_t get_file_size(int fd)
{
    struct stat st;

    if (fstat(fd, &st) < 0) {
        perror("fstat");
        return -1;
    }
    if (S_ISBLK(st.st_mode)) {
        unsigned long long bytes;
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
            perror("ioctl");
            return -1;
        }
        return bytes;
    }
    else if (S_ISREG(st.st_mode))
        return st.st_size;

    return -1;
}

/* we initialize our user space ring buffers while kernel setting up theirs */
int app_setup_uring(struct submitter *s)
{
    struct app_io_sq_ring *sring = &s->sq_ring;
    struct app_io_cq_ring *cring = &s->cq_ring;
    struct io_uring_params params;
    void *sq_ptr, *cq_ptr;

    memset(&params, 0, sizeof(params));
    /* setup io_urings and io_uring_params on the kernel space */
    s->ring_fd = io_uring_setup(QUEUE_DEPTH, &params);
    if (s->ring_fd < 0) {
        perror("io_uring_setup");
        return 1;
    }

    int sring_size = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    int cring_size = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);

    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        if (cring_size > sring_size)
            sring_size = cring_size;
        cring_size = sring_size;
    }

    sq_ptr = mmap(0, sring_size, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE,
        s->ring_fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /*
     * if kernel supports single mmap then we just mmap sq ring only
     * NOTE: this feature is available in kernel only above 5.4
     */
    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        cq_ptr = sq_ptr;
    } else {
        cq_ptr = mmap(0, cring_size, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE,
            s->ring_fd, IORING_OFF_CQ_RING);
        if (cq_ptr == MAP_FAILED) {
            perror("mmap");
            return 1;
        }
    }
    /* useful fields */
    sring->head = sq_ptr + params.sq_off.head;
    sring->tail = sq_ptr + params.sq_off.tail;
    sring->ring_mask = sq_ptr + params.sq_off.ring_mask;
    sring->ring_entries = sq_ptr + params.sq_off.ring_entries;
    sring->flags = sq_ptr + params.sq_off.flags;
    sring->array = sq_ptr + params.sq_off.array;

    s->sqes = mmap(0, params.sq_entries * sizeof(struct io_uring_sqe),
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
        s->ring_fd, IORING_OFF_SQES);
    if (s->sqes == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    /* useful fields (again) */
    cring->head = cq_ptr + params.cq_off.head;
    cring->tail = cq_ptr + params.cq_off.tail;
    cring->ring_mask = cq_ptr + params.cq_off.ring_mask;
    cring->ring_entries = cq_ptr + params.cq_off.ring_entries;
    cring->cqes = cq_ptr + params.cq_off.cqes;

    return 0;
}

/* put some characters to the console with buffered output */
void output_to_console(char *buf, int len)
{
    while (len--)
        fputc(*buf++, stdout);
}

void read_from_cq(struct submitter *s)
{
    struct file_info *fi;
    struct app_io_cq_ring *cring = &s->cq_ring;
    struct io_uring_cqe *cqe;
    unsigned head, reaped = 0;

    head = *cring->head;

    do {
        read_barrier();
        /* no entries left; has nothing to read */
        if (head == *cring->tail)
            break;
        /* get entry from cq */
        cqe = &cring->cqes[head & *s->cq_ring.ring_mask];
        fi = (struct file_info*) cqe->user_data;
        if (cqe->res < 0)
            fprintf(stderr, "Error: %s\n", strerror(abs(cqe->res)));

        int blocks = (int) fi->file_size / FILE_BLOCK_SIZE;
        if (fi->file_size % FILE_BLOCK_SIZE)
            blocks++;
        /* print out the data */
        for (int i = 0; i < blocks; i++)
            output_to_console(fi->iovecs[i].iov_base, fi->iovecs[i].iov_len);
        head++;
    } while (1);

    *cring->head = head;
    write_barrier();
}

int submit_to_sq(char *file_path, struct submitter *s)
{
    struct file_info *fi;

    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        perror("open");
        return 1;
    }

    struct app_io_sq_ring *sring = &s->sq_ring;
    unsigned index = 0, current_block = 0, tail = 0, next_tail = 0;

    off_t file_size = get_file_size(file_fd);
    if (file_size < 0)
        return 1;
    off_t bytes_remaining = file_size;
    int blocks = (int) file_size / FILE_BLOCK_SIZE;
    if (file_size % FILE_BLOCK_SIZE)
        blocks++;

    fi = malloc(sizeof(*fi) + sizeof(struct iovec) * blocks);
    if (!fi) {
        fprintf(stderr, "Unable to allocate memory\n");
        return 1;
    }
    fi->file_size = file_size;
    /* read file data into the buffer (iovecs) */
    while (bytes_remaining) {
        off_t bytes_to_read = bytes_remaining;
        if (bytes_to_read > FILE_BLOCK_SIZE)
            bytes_to_read = FILE_BLOCK_SIZE;

        /* we need to allocate iovec array block by block */
        fi->iovecs[current_block].iov_len = bytes_to_read;
        void *buf = aligned_alloc(FILE_BLOCK_SIZE, FILE_BLOCK_SIZE);
        if (buf == NULL) {
            perror("aligned_alloc");
            return 1;
        }
        fi->iovecs[current_block].iov_base = buf;

        current_block++;
        bytes_remaining -= bytes_to_read;
    }
    /* initialization of sq entry */
    next_tail = tail = *sring->tail; /* what do these tails represent of?? */
    next_tail++;
    read_barrier();
    index = tail & *s->sq_ring.ring_mask;
    struct io_uring_sqe *sqe = &s->sqes[index];
    sqe->fd = file_fd;
    sqe->flags = 0;
    sqe->opcode = IORING_OP_READV;
    sqe->addr = (unsigned long) fi->iovecs;
    sqe->len = blocks;
    sqe->off = 0;
    sqe->user_data = (unsigned long long) fi;
    sring->array[index] = index;
    tail = next_tail;
    /* update tail */
    if (*sring->tail != tail) {
        *sring->tail = tail;
        write_barrier();
    }
    /* submit sq to kernel with io_uring_enter() system call */
    int ret = io_uring_enter(s->ring_fd, 1, 1, IORING_ENTER_GETEVENTS);
    if (ret < 0) {
        perror("io_uring_enter");
        return 1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    struct submitter *s;
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <filename1> [<filename2>, ...]\n", argv[0]);
        return 1;
    }

    s = malloc(sizeof(*s));
    if (!s) {
        perror("malloc");
        return 1;
    }
    memset(s, 0, sizeof(*s));

    /* setup urings */
    if (app_setup_uring(s)) {
        fprintf(stderr, "Unable to setup uring!\n");
        return 1;
    }

    /* put the file into sq, read the result from cq */
    for (int i = 1; i < argc; i++) {
        if (submit_to_sq(argv[i], s)) {
            fprintf(stderr, "Error reading file\n");
            return 1;
        }
        read_from_cq(s);
    }

    return 0;
}
