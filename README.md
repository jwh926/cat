# cat
simple implementation of linux command `cat` using io_uring

## how to compile
```sh
gcc ./cat_with_io_uring.c -o cat
```

run the program just like regular `cat`: `./cat <file>`

## Code Overview

This implementation demonstrates how to use Linux's io_uring interface for asynchronous I/O operations to read and display file contents. The program uses a low-level approach by directly interfacing with the kernel's io_uring system calls.

### Key Concepts

**io_uring** is a Linux kernel interface that provides efficient asynchronous I/O operations through a pair of ring buffers:
- **Submission Queue (SQ)**: Where the application submits I/O requests
- **Completion Queue (CQ)**: Where the kernel places completed I/O results

### Data Structures

#### `struct app_io_sq_ring`
Represents the user-space submission queue ring buffer with pointers to:
- `head`, `tail`: Ring buffer position indicators
- `ring_mask`, `ring_entries`: Ring buffer size and masking information
- `flags`: Control flags for the ring
- `array`: Array of submission queue entry indices

#### `struct app_io_cq_ring`  
Represents the user-space completion queue ring buffer with pointers to:
- `head`, `tail`: Ring buffer position indicators
- `ring_mask`, `ring_entries`: Ring buffer size and masking information
- `cqes`: Array of completion queue entries

#### `struct submitter`
Main context structure containing:
- `ring_fd`: File descriptor for the io_uring instance
- `sq_ring`: Submission queue ring buffer
- `sqes`: Array of submission queue entries
- `cq_ring`: Completion queue ring buffer

#### `struct file_info`
Holds file data and metadata:
- `file_size`: Total size of the file
- `iovecs[]`: Array of I/O vectors for scatter-gather operations

### Key Functions

#### `app_setup_uring(struct submitter *s)`
Initializes the io_uring interface:
1. Calls `io_uring_setup()` system call to create io_uring instance
2. Maps ring buffers into user space using `mmap()`
3. Sets up pointers to ring buffer fields using kernel-provided offsets
4. Handles both single-mmap and dual-mmap modes for compatibility

#### `submit_to_sq(char *file_path, struct submitter *s)`
Submits a file read request to the submission queue:
1. Opens the file and determines its size
2. Allocates memory for file data in 4KB blocks using `aligned_alloc()`
3. Creates I/O vectors (`iovecs`) for each block
4. Prepares a submission queue entry (`io_uring_sqe`) with:
   - File descriptor
   - Operation type (`IORING_OP_READV` for vectored read)
   - Buffer addresses and sizes
   - User data pointer for result correlation
5. Updates the submission queue tail and submits via `io_uring_enter()`

#### `read_from_cq(struct submitter *s)`
Processes completed I/O operations from the completion queue:
1. Reads completion queue entries in order
2. Retrieves file data using the user data pointer
3. Outputs file contents to stdout block by block
4. Updates completion queue head to mark entries as consumed

#### `output_to_console(char *buf, int len)`
Simple output function that writes characters to stdout one by one.

### Program Flow

1. **Initialization**: Allocate submitter structure and setup io_uring rings
2. **File Processing**: For each command-line argument:
   - Submit file read request to submission queue
   - Wait for completion and read results from completion queue
   - Output file contents to console
3. **Cleanup**: Implicit cleanup when program exits

### Memory Management

- Files are read in 4KB blocks (`FILE_BLOCK_SIZE`) for efficient I/O
- Uses `aligned_alloc()` for proper memory alignment required by io_uring
- Employs scatter-gather I/O with `iovec` structures for handling large files
- Ring buffers are memory-mapped from kernel space for zero-copy operation

### Synchronization

The code uses memory barriers (`read_barrier()` and `write_barrier()`) implemented as compiler memory barriers to ensure proper ordering of memory operations when accessing shared ring buffers between user and kernel space.

### Error Handling

- System call failures are handled with `perror()` for diagnostic output
- File operations include proper error checking and cleanup
- I/O completion errors are reported through completion queue entry results
