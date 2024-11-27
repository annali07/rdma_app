#include <netinet/in.h>
#ifndef RDMA_CLIENT_H
#define RDMA_CLIENT_H

struct client_param {
    char server_ip[INET_ADDRSTRLEN];
    char *ib_devname;
    int ib_port;
    int queue_depth;
    int num_jobs;
};

struct job_context {
    int job_id;         // 0 - TOTAL_JOBS
    void *buffer;        // RDMA buffer for this job
    struct ibv_mr *mr;  // MR for this buffer
    struct timespec start_time;
    struct timespec end_time;
    bool in_flight;
    uint32_t page_id;
};

struct client_context {
    bool job_completed[MAX_JOBS];   
    struct job_context contexts[MAX_QUEUE_DEPTH];
    int next_job_to_send;       // 0 - TOTAL_JOBS
    int jobs_completed;

    struct ibv_qp *qp;
    struct ibv_cq *cq;
    struct ibv_pd *pd;

    void *recv_buffer;
    struct ibv_mr *recv_mr;     // for receiving page_id

    uint64_t remote_addr;
    uint32_t remote_rkey;
};

// Function declarations
bool all_jobs_completed(struct client_context *ctx, int num_jobs);
void poll_completions(struct client_context *state, int buf_size, int queue_depth, int num_jobs);
int post_rdma_write_imm(struct client_context *state, int buf_index, int buf_size);
void prepare_job_data(void *buffer, int job_id);
void send_next_job(struct client_context *state, int ctx_index, int buf_size);
void handle_pageid_received(struct client_context *state, 
                          struct ibv_wc *wc,
                          int buf_size, 
                          int queue_depth,
                          int num_jobs);
int post_receive(struct client_context *ctx, int recv_idx);
void run_client(struct client_context *ctx, int queue_depth, int num_jobs, int buf_size);
struct client_context* setup_client(int queue_depth, int buf_size);
int parser_client(struct client_param *user_param, char *argv[], int argc);
void cleanup_client_context(struct client_context *ctx, int queue_depth);

#endif