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

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = inet_addr(server_ip);

    // if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
    //     perror("Invalid address");
    //     close(sockfd);
    //     return FAILURE;
    // }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("Server IP: %s\n", server_ip);
        perror("Failed to connect to server");
        close(sockfd);
        return FAILURE;
    }

    printf("Connected to server\n");
    return sockfd;
}

int exchange_connection_info (struct connection_info *local_info, struct connection_info *remote_info, char server_ip[INET_ADDRSTRLEN]) {
    int sockfd = create_client_socket(server_ip);
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
    info->gid = res->gid;
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

    printf("Writing to remote address: %lx, offset: %x, buf_index: %d, buf_size: %d\n",
      state->remote_addr + (buf_size * buf_index), buf_size * buf_index, buf_index, buf_size);

    struct ibv_send_wr wr = {
        .wr_id = buf_index,
        .sg_list = &sg,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
        .send_flags = IBV_SEND_SIGNALED,
        .imm_data = (uint32_t) buf_index,
        .wr.rdma = {
            .remote_addr = state->remote_addr + (buf_size * buf_index),
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

int setup_client(struct client_context *ctx, struct rdma_resources *res, int queue_depth, int buf_size) {

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
        ctx->contexts[i].mr = ibv_reg_mr(res->pd, ctx->contexts[i].buffer, buf_size,
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
    ctx->recv_mr = ibv_reg_mr(res->pd, ctx->recv_buffer, queue_depth * sizeof(uint32_t),
                                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!ctx->recv_mr) {
        perror("Failed to register memory for recv buffer\n");
        free(ctx->recv_buffer);
        goto cleanup;
    }

    return SUCCESS;

cleanup:
    for (int i = 0; i < queue_depth; ++i) {
        if (ctx->contexts[i].mr) 
            ibv_dereg_mr(ctx->contexts[i].mr);
        if (ctx->contexts[i].buffer) 
            free(ctx->contexts[i].buffer);
    }
    free(ctx);
    return FAILURE;
}

int parser_client(struct client_param *user_param, char *argv[], int argc)
{
    if (argc < 7) {
        fprintf(stderr, "USAGE: %s <server_ip> <ibv_device> <queue_depth> <num_jobs> <batch_size> <num_threads>\n", argv[0]);
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

    user_param->batch_size = atoi(argv[5]);
    user_param->num_threads = atoi(argv[6]);

    user_param->ib_port = IB_PORT_DEFAULT;
    return SUCCESS;
}

void *worker_thread(void *arg) {
    struct client_context *ctx = (struct client_context *) arg;
    struct rdma_resources *rdma_res = NULL;
    struct connection_info *local_info = NULL;
    struct connection_info *remote_info = NULL;

    // initialize rdma resources
    printf("Starting rdma_init_resources...\n");
    rdma_res = rdma_init_resources(ctx->config);
    if (!rdma_res) {
        fprintf(stderr, "Failed to initialize RDMA resources\n");
        goto cleanup;
    }
    printf("RDMA resources initialized\n");

    printf("Setting up client context...\n");
    if (setup_client(ctx, rdma_res, ctx->queue_depth, ctx->buf_size) < 0){
        fprintf(stderr, "Failed to setup client context\n");
        goto cleanup;
    }

    // set rdma resources in client ctx
    ctx->qp = rdma_res->qp;
    ctx->cq = rdma_res->cq;
    ctx->pd = rdma_res->pd;

    // change qp to init
    if (change_qpstate_to_init(rdma_res, ctx->config) != SUCCESS) {
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
    if (exchange_connection_info(local_info, remote_info, ctx->server_ip) != SUCCESS) {
        fprintf(stderr, "Failed to exchange connection info\n");
        goto cleanup;
    }

    // save remote buf info 
    ctx->remote_addr = remote_info->addr;
    ctx->remote_rkey = remote_info->rkey;

    // change state to RTR
    if (change_qp_to_RTR(rdma_res, remote_info, ctx->config) != SUCCESS) {
        fprintf(stderr, "Failed to change QP state to RTR\n");
        goto cleanup;
    }

    // change qp to rts
    if (change_qp_to_RTS(rdma_res) != SUCCESS) {
        fprintf(stderr, "Failed to change QP state to RTS\n");
        goto cleanup;
    }
    
    sleep(1);

    // run client main loop
    run_client(ctx, ctx->queue_depth, ctx->num_jobs,ctx->buf_size);
cleanup:
    if (local_info) free(local_info);
    if (remote_info) free(remote_info);
    if (rdma_res) rdma_free_resources(rdma_res);
}


int main(int argc, char *argv[]) {
    
    struct client_param *user_param = NULL;
    struct main_client_context *main_client_ctx;
    pthread_t *thread_handles;
    int ret = FAILURE;


    // parse arguments
    user_param = malloc(sizeof(struct client_param));
    if (!user_param || parser_client(user_param, argv, argc) != SUCCESS) {
        fprintf(stderr, "Failed to parse parameters\n");
        goto cleanup;
    }

    int buf_size = PAGE_SIZE * user_param->batch_size;
    int tasks_per_thread = user_param->num_jobs / user_param->num_threads;

    main_client_ctx = malloc(sizeof(struct main_client_context));
    if (!main_client_ctx) {
        perror("Failed to allocate memory for main client context");
        goto cleanup;
    }

    main_client_ctx->num_threads = user_param->num_threads;
    main_client_ctx->params = user_param;
    main_client_ctx->threads = malloc(sizeof(struct client_context*) * user_param->num_threads);
    thread_handles = malloc(sizeof(pthread_t) * user_param->num_threads);
    if (!main_client_ctx->threads || !thread_handles) {
        perror("Failed to allocate memory for threads");
        goto cleanup;
    }
    printf("num trheads: %d", user_param->num_threads);

    for (int i = 0; i < user_param->num_threads; ++i) {
        struct client_context *thread_ctx = malloc(sizeof(struct client_context));
        if (!thread_ctx) {
            perror("Failed to allocate memory for thread context");
            goto cleanup;
        }
        thread_ctx->num_jobs = tasks_per_thread;
        thread_ctx->buf_size = buf_size;
        thread_ctx->thread_id = i;
        strcpy(thread_ctx->server_ip, user_param->server_ip);

        struct rdma_config *config = malloc(sizeof(struct rdma_config));
        config->ib_devname = malloc(sizeof(char) * IBV_DEVICE_MAX_LENGTH);
        if (!config->ib_devname) {
            free(config);  // Clean up first malloc
            perror("Malloc failed\n");
            exit(FAILURE);
        }
        config->ib_port = IB_PORT_DEFAULT;
        config->cq_size = MAX_QUEUE_DEPTH;
        config->num_qp_wr = MAX_QUEUE_DEPTH;
        config->num_sge = 1;
        config->use_event = false;

        strncpy(config->ib_devname, user_param->ib_devname, IBV_DEVICE_MAX_LENGTH - 1);
        config->ib_devname[IBV_DEVICE_MAX_LENGTH - 1] = '\0';

        thread_ctx->config = config;
        thread_ctx->queue_depth = user_param->queue_depth;

        main_client_ctx->threads[i] = thread_ctx;
        if (pthread_create(&thread_handles[i], NULL, worker_thread, thread_ctx)) {
            perror("Failed to create thread");
            goto cleanup;
        }
    }

    for (int i = 0; i < user_param->num_threads; i++) {
        void *thread_ret;
        pthread_join(thread_handles[i], &thread_ret);
    }

    ret = SUCCESS;

cleanup:
    if (thread_handles) {
        free(thread_handles);
    }
    if (main_client_ctx) {
        cleanup_main_context(main_client_ctx);
    } else {
        // If main_client_ctx wasn't allocated but user_param was
        if (user_param) {
            if (user_param->ib_devname) free(user_param->ib_devname);
            free(user_param);
        }
    }
    
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

void cleanup_main_context(struct main_client_context *main_ctx) {
    if (!main_ctx) return;
    
    // Clean up threads array
    if (main_ctx->threads) {
        for (int i = 0; i < main_ctx->num_threads; i++) {
            if (main_ctx->threads[i]) {
                // Clean up config
                if (main_ctx->threads[i]->config) {
                    if (main_ctx->threads[i]->config->ib_devname) {
                        free(main_ctx->threads[i]->config->ib_devname);
                    }
                    free(main_ctx->threads[i]->config);
                }
                cleanup_client_context(main_ctx->threads[i], main_ctx->threads[i]->queue_depth);
            }
        }
        free(main_ctx->threads);
    }
    
    // Clean up params
    if (main_ctx->params) {
        if (main_ctx->params->ib_devname) {
            free(main_ctx->params->ib_devname);
        }
        free(main_ctx->params);
    }
    
    free(main_ctx);
}