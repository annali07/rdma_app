# include <unistd.h>
# include <stdbool.h>

typedef struct compressed_result {
    void *data;           // Pointer to compressed data
    size_t orig_size;     // Original size before compression
    size_t comp_size;     // Size after compression
    double ratio;         // Compression ratio
    int status;          // Compression status/error code
} compressed_result_t; // TODO: MODIFY


struct server_param {
    char* ib_devname;
    int ib_port;
    int queue_depth;
    int num_jobs;
};

struct buffer_entry {
    void *buffer;
    struct ibv_mr *mr;
    bool in_use;        // irrelevant except for error detection purposes 
    uint32_t job_id;    // completely not used
};

struct server_context {
    // one buffer per possible in-flight request 
    struct buffer_entry buffers[MAX_QUEUE_DEPTH];

    uint32_t *page_id_send;    // Single uint32_t buffer
    struct ibv_mr *page_id_mr; // MR for the page_id buffer

    uint64_t remote_addr;
    uint32_t remote_rkey;

    // RDMA resources
    struct ibv_qp *qp;
    struct ibv_cq *cq;
    struct ibv_pd *pd;

    // next page_id to assign
    uint32_t next_page_id;      // TODO: Modify based on compression logic
};

struct server_context *setup_server(struct rdma_resources *res, int queue_depth, int buf_size);
void run_server(struct server_context *ctx, int queue_depth, int buf_size);
void post_receive_for_buffer(struct server_context *ctx, int buf_index, int buf_size);
void process_received_job(struct server_context *ctx, uint32_t buf_index, int buf_size);
void handle_pageid_sent(struct server_context *ctx, struct ibv_wc *wc, int buf_size);
void on_compression_complete(void *context, int buf_index, compressed_result_t *result);
void store_compressed_data(uint32_t page_id, compressed_result_t *result);\
void prepare_job_data(void *buffer, int job_id);
void post_rdma_write_imm(struct server_context *ctx, int buf_index, int buf_size);
struct connection_info *create_connection_info(struct rdma_resources *res, struct server_context *ctx);
int create_server_socket();
int parse_arg(struct server_param *user_param, char *argv[], int argc);
void cleanup_server_context(struct server_context *ctx, int queue_depth);
