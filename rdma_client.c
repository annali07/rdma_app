#define _POSIX_C_SOURCE 199309L

#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> 
#include <stdbool.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h> 

#include "common.h"
#include "rdma_client.h"

#define PORT 8080

int create_client_socket(const char *server_ip) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Failed to create socket");
        return FAILURE;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
    };

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(sockfd);
        return FAILURE;
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Failed to connect to server");
        close(sockfd);
        return FAILURE;
    }

    return sockfd;
}

int exchange_connection_info (struct connection_info *local_info, struct connection_info *remote_info, struct client_param *user_param) {
    int sockfd = create_client_socket(user_param->server_ip);
    if (sockfd < 0) {
        fprintf(stderr, "Failed to create client socket\n");
        return FAILURE;
    }

    if (write(sockfd, local_info, sizeof(*local_info)) < 0) {
        perror("Failed to send connection info");
        close(sockfd);
        return FAILURE;
    }

    // receive server info 
    if (read(sockfd, remote_info, sizeof(*remote_info)) < 0) {
        perror("Failed to receive connection info");
        close(sockfd);
        return FAILURE;
    }
    
    close(sockfd);
    return SUCCESS;
}

struct connection_info * create_connection_info(struct rdma_resources *res, struct client_context *ctx) {
    if (!res) {
        fprintf(stderr, "Invalid RDMA resources\n");
        return NULL;
    }

    struct connection_info *info = malloc(sizeof(*info));
    if (!info) {
        perror("Failed to allocate connection info");
        return NULL;
    }

    // Fill in connection details from rdma_resources
    info->lid = res->lid;
    info->qpn = res->qp_num;
    info->rkey = (uint32_t) ctx->recv_mr->rkey;
    info->addr = (uint64_t) ctx->recv_buffer;

    return info;
}

bool all_jobs_completed(struct client_context *ctx, int num_jobs) {
    return ctx->jobs_completed >= num_jobs;
}

double compute_latency (struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 + (end->tv_nsec - start->tv_nsec) / 1e6;
}

void poll_completions(struct client_context *state, int buf_size, int queue_depth, int num_jobs) {
    struct ibv_wc wc;
    int ne;

    ne = ibv_poll_cq(state->cq, 1, &wc);
    if (ne < 0) {
        fprintf(stderr, "Failed to poll CQ\n");
        return;
    }
    if (ne == 0) {
        return; // nothing completed
    }
    
    if(wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                ibv_wc_status_str(wc.status), wc.status, (int)wc.wr_id);
        return;
    }

    if (wc.opcode == IBV_WC_RDMA_WRITE) {
        // the write_imm sent successfully
        printf("Send completed successfully\n");
    }
    else if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
        // the receive job completed
        handle_pageid_received(state, &wc, buf_size, queue_depth, num_jobs);
    }
}

// function to post RDMA Write with Immediate 
int post_rdma_write_imm(struct client_context *state, int buf_index, int buf_size) {
    struct job_context *job = &state->contexts[buf_index];

    struct ibv_sge sg = {
        .addr = (uint64_t)job->buffer,
        .length = buf_size,
        .lkey = job->mr->lkey
    };

    struct ibv_send_wr wr = {
        .wr_id = buf_index,
        .sg_list = &sg,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
        .send_flags = IBV_SEND_SIGNALED,
        .imm_data = (uint32_t) buf_index,
        .wr.rdma = {
            .remote_addr = (uint64_t)((char*)state->remote_addr + buf_size * buf_index),
            .rkey = state->remote_rkey
        }
    };
    struct ibv_send_wr *bad_wr;
    return ibv_post_send(state->qp, &wr, &bad_wr);    
}

void prepare_job_data(void *buffer, int job_id) {
    (void) buffer;
    (void) job_id;
    
    // TODO:
}

void send_next_job(struct client_context *state, int ctx_index, int buf_size) {
    struct job_context *ctx = &state->contexts[ctx_index];

    // prepare job data in buffer 
    prepare_job_data(ctx->buffer, state->next_job_to_send);
    printf("Sending job %d\n", state->next_job_to_send);

    // record ctx info
    ctx->job_id = state->next_job_to_send;
    ctx->in_flight = true;  
    clock_gettime(CLOCK_MONOTONIC, &ctx->start_time);

    post_rdma_write_imm(state, ctx_index, buf_size); // use buf id as imm data
    state->next_job_to_send++;
}

void handle_pageid_received (struct client_context *state, struct ibv_wc *wc, int buf_size, int queue_depth, int num_jobs) {
    // find which context this completion belongs to in state
    int recv_idx = wc->imm_data;
    if (recv_idx < 0 || recv_idx >= queue_depth) {
        fprintf(stderr, "Invalid recv_idx %d retrieved from CQ\n", recv_idx);
        return;
    }
    struct job_context *job_ctx = &state->contexts[recv_idx];

    // record page_id and completion time
    uint32_t* buffer_ptr = (uint32_t*)state->recv_buffer + recv_idx;
    job_ctx->page_id = *buffer_ptr;
    clock_gettime(CLOCK_MONOTONIC, &job_ctx->end_time);

    // calculate and log latency
    double latency = compute_latency(&job_ctx->start_time, &job_ctx->end_time);
    printf("Job %d completed with page_id %u and latency %.2f ms\n",
           job_ctx->job_id, job_ctx->page_id, latency);

    // mark job as completed
    state->job_completed[job_ctx->job_id] = true;
    state->jobs_completed++;
    job_ctx->in_flight = false;

    // post new receive for this buffer 
    post_receive(state, recv_idx);

    // send next job if any remaining
    if (state->next_job_to_send < num_jobs) {
        send_next_job(state, recv_idx, buf_size);
    }
}

int post_receive(struct client_context *ctx, int recv_idx) {
    struct ibv_sge sg = {
        .addr = (uint64_t) ((uint32_t*) ctx->recv_buffer + recv_idx),
        .length = sizeof(uint32_t),
        .lkey = ctx->recv_mr->lkey
    };

    struct ibv_recv_wr wr = {
        .wr_id = recv_idx,
        .sg_list = &sg,
        .num_sge = 1
    };

    struct ibv_recv_wr *bad_wr;
    return ibv_post_recv(ctx->qp, &wr, &bad_wr);
}

////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////

void run_client(struct client_context *ctx, int queue_depth, int num_jobs, int buf_size) {
    // post initial receives for page_ids
    for (int i = 0; i < queue_depth; ++i ) {
        post_receive(ctx, i);
    }

    // send initial batch of jobs
    for (int i = 0; i < queue_depth && ctx->next_job_to_send < num_jobs; ++i) {
        send_next_job(ctx, i, buf_size);
    }

    // main loop
    while (!all_jobs_completed(ctx, num_jobs)) {
        poll_completions(ctx, buf_size, queue_depth, num_jobs);
    }
}

struct client_context *setup_client(int queue_depth, int buf_size) {
    struct client_context *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        fprintf(stderr, "Failed to allocate memory for client state\n");
        return NULL;
    }

    memset(ctx->job_completed, 0, sizeof(ctx->job_completed));
    ctx->jobs_completed = 0;
    ctx->next_job_to_send = 0;

    // Set up RDMA Resources

    // Setup contexts and buffers (queue depth of buffers allocated)
    for (int i = 0; i < queue_depth; ++i ) {
        ctx->contexts[i].buffer = malloc(buf_size);
        if (!ctx->contexts[i].buffer) {
            fprintf(stderr, "Failed to allocate memory for context buffer %d\n", i);
            goto cleanup;
        }
        ctx->contexts[i].mr = ibv_reg_mr(ctx->pd, ctx->contexts[i].buffer, buf_size,
                                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        if (!ctx->contexts[i].mr) {
            fprintf(stderr, "Failed to register memory for context buffer %d\n", i);
            free(ctx->contexts[i].buffer);
            goto cleanup;
        }

        ctx->contexts[i].in_flight = false;
    }

    // set up recv buffer for page_id
    ctx->recv_buffer = malloc(queue_depth * sizeof(uint32_t));
    if (!ctx->recv_buffer) {
        perror("Failed to allocate memory for recv buffer\n");
        goto cleanup;
    }
    ctx->recv_mr = ibv_reg_mr(ctx->pd, ctx->recv_buffer, queue_depth * sizeof(uint32_t),
                                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!ctx->recv_mr) {
        perror("Failed to register memory for recv buffer\n");
        free(ctx->recv_buffer);
        goto cleanup;
    }

    return ctx;

cleanup:
    for (int i = 0; i < queue_depth; ++i) {
        if (ctx->contexts[i].mr) 
            ibv_dereg_mr(ctx->contexts[i].mr);
        if (ctx->contexts[i].buffer) 
            free(ctx->contexts[i].buffer);
    }
    free(ctx);
    return NULL;
}

int parser_client(struct client_param *user_param, char *argv[], int argc)
{
    if (argc < 5) {
        fprintf(stderr, "USAGE: %s <server_ip> <ibv_device> <queue_depth> <num_jobs>\n", argv[0]);
        return FAILURE;
    }
    // Assign server_ip and ib_devname from argv
    char *server_ip = argv[1];
    if (validate_ip(server_ip) != SUCCESS) {
        return FAILURE;
    }
    // populate user_param struct
    strncpy(user_param->server_ip, server_ip, INET_ADDRSTRLEN - 1);
    user_param->server_ip[INET_ADDRSTRLEN - 1] = '\0'; // Ensure null-termination

    user_param->ib_devname = malloc(IBV_DEVICE_MAX_LENGTH);
    if (!user_param->ib_devname) {
        perror("Malloc failed for devname");
        return FAILURE;
    }
    strncpy(user_param->ib_devname, argv[2], IBV_DEVICE_MAX_LENGTH - 1);
    user_param->ib_devname[IBV_DEVICE_MAX_LENGTH - 1] = '\0';
    
    user_param->queue_depth = atoi(argv[3]);
    if (user_param->queue_depth <= 0 || user_param->queue_depth > MAX_QUEUE_DEPTH) {
        fprintf(stderr, "Invalid queue depth (1-%d): %d\n", MAX_QUEUE_DEPTH, user_param->queue_depth);
        return FAILURE;
    }

    user_param->num_jobs = atoi(argv[4]);
    if (user_param->num_jobs <= 0 || user_param->num_jobs > MAX_JOBS) {
        fprintf(stderr, "Invalid number of jobs (1-%d): %d\n", MAX_JOBS, user_param->num_jobs);
        return FAILURE;
    }
    user_param->ib_port = IB_PORT_DEFAULT;
    return SUCCESS;
}

int main(int argc, char *argv[]) {
    struct rdma_resources *rdma_res = NULL;
    struct connection_info *local_info = NULL;
    struct connection_info *remote_info = NULL;
    struct client_param *user_param = NULL;
    struct client_context *ctx = NULL;
    int ret = -1;

    // init config 
    struct rdma_config config = {
        .ib_devname = NULL,
        .ib_port = IB_PORT_DEFAULT,
        .cq_size = MAX_QUEUE_DEPTH,
        .num_qp_wr = MAX_QUEUE_DEPTH,
        .num_sge = 1,
        .use_event = false      
    };
    config.ib_devname = malloc(sizeof(char) * IBV_DEVICE_MAX_LENGTH);
    if (!config.ib_devname) {
        perror("Malloc failed\n");
        exit(FAILURE);
    }

    const int buf_size = PAGE_SIZE;

    // parse arguments
    user_param = malloc(sizeof(struct client_param));

    if (!user_param || parser_client(user_param, argv, argc) != SUCCESS) {
        fprintf(stderr, "Failed to parse parameters\n");
        goto cleanup;
    }
    strncpy(config.ib_devname, user_param->ib_devname, IBV_DEVICE_MAX_LENGTH - 1);
    config.ib_devname[IBV_DEVICE_MAX_LENGTH - 1] = '\0';
    printf("Device name: %s\n", config.ib_devname);
    
    // initialize rdma resources
    printf("Starting rdma_init_resources...\n");
    rdma_res = rdma_init_resources(&config);
    if (!rdma_res) {
        fprintf(stderr, "Failed to initialize RDMA resources\n");
        goto cleanup;
    }
    printf("RDMA resources initialized\n");

    printf("Setting up server context...\n");
    // create client context
    ctx = setup_client(user_param->queue_depth, buf_size);
    if (!ctx) {
        fprintf(stderr, "Failed to setup client context\n");
        goto cleanup;
    }

    // set rdma resources in client ctx
    ctx->qp = rdma_res->qp;
    ctx->cq = rdma_res->cq;
    ctx->pd = rdma_res->pd;

    // change qp to init
    if (change_qpstate_to_init(rdma_res, &config) != SUCCESS) {
        fprintf(stderr, "Failed to change QP state to INIT\n");
        goto cleanup;
    }

    // create connection info
    local_info = create_connection_info(rdma_res, ctx);
    remote_info = malloc(sizeof(*remote_info));
    if (!remote_info || !local_info) {
        fprintf(stderr, "Failed to create connection info\n");
        goto cleanup;
    }

    // connect to server and exchange info 
    if (exchange_connection_info(local_info, remote_info, user_param) != SUCCESS) {
        fprintf(stderr, "Failed to exchange connection info\n");
        goto cleanup;
    }

    // save remote buf info 
    ctx->remote_addr = remote_info->addr;
    ctx->remote_rkey = remote_info->rkey;

    // change state to RTR
    if (change_qp_to_RTR(rdma_res, remote_info, &config) != SUCCESS) {
        fprintf(stderr, "Failed to change QP state to RTR\n");
        goto cleanup;
    }

    // change qp to rts
    if (change_qp_to_RTS(rdma_res) != SUCCESS) {
        fprintf(stderr, "Failed to change QP state to RTS\n");
        goto cleanup;
    }
    
    // run client main loop
    run_client(ctx, user_param->queue_depth, user_param->num_jobs, buf_size);
    ret = 0;

cleanup:
    if (ctx) cleanup_client_context(ctx, user_param ? user_param->queue_depth : 0);
    if (rdma_res) rdma_free_resources(rdma_res);
    if (local_info) free(local_info);
    if (remote_info) free(remote_info);
    if (user_param) free(user_param);
    return ret;
}

void cleanup_client_context(struct client_context *ctx, int queue_depth) {
    if (!ctx) return;
    
    // Cleanup contexts
    for (int i = 0; i < queue_depth; i++) {
        if (ctx->contexts[i].mr) 
            ibv_dereg_mr(ctx->contexts[i].mr);
        if (ctx->contexts[i].buffer) 
            free(ctx->contexts[i].buffer);
    }
    
    // Cleanup receive buffer
    if (ctx->recv_mr) 
        ibv_dereg_mr(ctx->recv_mr);
    if (ctx->recv_buffer) 
        free(ctx->recv_buffer);
        
    free(ctx);
}
