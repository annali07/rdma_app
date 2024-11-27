#include <infiniband/verbs.h>
#include <stdio.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdlib.h>       // for malloc, free, atoi
#include <string.h>       // for strncpy

#include "common.h"
#include "rdma_server.h"

#define PORT 8080

// server arg parser
int parse_arg(struct server_param *user_param, char *argv[], int argc)
{
    if (argc < 4) {
        fprintf(stderr, "USAGE: %s <ibv_device> <queue_depth> <num_jobs>\n", argv[0]);
        return FAILURE;
    }

    user_param->ib_devname = malloc(IBV_DEVICE_MAX_LENGTH);
    if (!user_param->ib_devname) {
        perror("Malloc failed for devname");
        return FAILURE;
    }
    strncpy(user_param->ib_devname, argv[1], IBV_DEVICE_MAX_LENGTH - 1);
    user_param->ib_devname[IBV_DEVICE_MAX_LENGTH - 1] = '\0';

    user_param->queue_depth = atoi(argv[2]);
    if (user_param->queue_depth <= 0 || user_param->queue_depth > MAX_QUEUE_DEPTH) {
        fprintf(stderr, "Invalid queue depth (1-%d): %d\n", MAX_QUEUE_DEPTH, user_param->queue_depth);
        return FAILURE;
    }

    user_param->num_jobs = atoi(argv[3]);
    if (user_param->num_jobs <= 0 || user_param->num_jobs > MAX_JOBS) {
        fprintf(stderr, "Invalid number of jobs (1-%d): %d\n", MAX_JOBS, user_param->num_jobs);
        return FAILURE;
    }

    user_param->ib_port = IB_PORT_DEFAULT;
    return SUCCESS;
}

struct connection_info* create_connection_info(struct rdma_resources *res, struct server_context *ctx) {
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
    info->rkey = (uint32_t) ctx->buffers[0].mr->rkey;
    info->addr = (uint64_t) ctx->buffers[0].buffer;

    return info;
}

int create_server_socket() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Failed to create socket");
        return FAILURE;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT)
    };

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))){
        perror("Error in setsocket");
        close(sockfd);
        return FAILURE;
    }

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Failed to bind socket");
        close(sockfd);
        return FAILURE;
    }

    if (listen(sockfd, 10) < 0) {
        perror("Failed to listen on socket");
        close(sockfd);
        return FAILURE;
    }

    return sockfd;
}

int exchange_connection_info (struct connection_info *local_info, struct connection_info *remote_info) {
    int server_sockfd = create_server_socket();
    if (server_sockfd < 0) return FAILURE;

    printf("Waiting for client connection...\n");
    
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_sockfd = accept(server_sockfd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_sockfd < 0) {
        perror("Failed to accept client connection");
        close(server_sockfd);
        return FAILURE;
    }

    printf("Connected with client\n");

    // receive client info first 
    if (read(client_sockfd, remote_info, sizeof(*remote_info)) < 0) {
        perror("Failed to receive connection info");
        close(client_sockfd);
        close(server_sockfd);
        return FAILURE;
    }

    if (write(client_sockfd, local_info, sizeof(*local_info)) < 0) {
        perror("Failed to send connection info");
        close(client_sockfd);
        close(server_sockfd);
        return FAILURE;
    }
    
    close(client_sockfd);
    close(server_sockfd);
    return SUCCESS;
}

int main(int argc, char *argv[]) {
    struct rdma_resources *rdma_res = NULL;
    struct server_context *ctx = NULL;
    struct connection_info *local_info = NULL;
    struct connection_info *remote_info = NULL;
    struct server_param *user_param = NULL;
    int ret = -1;
    
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

    int buf_size = PAGE_SIZE;

    user_param = malloc(sizeof(struct server_param));

    if (!user_param || parse_arg(user_param, argv, argc) != SUCCESS) {
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
    // create server context
    ctx = setup_server(rdma_res, user_param->queue_depth, buf_size);
    if (!ctx) {
        fprintf(stderr, "Failed to setup server context\n");
        goto cleanup;
    }

    // Set RDMA resources in server context
    ctx->qp = rdma_res->qp;
    ctx->cq = rdma_res->cq;
    ctx->pd = rdma_res->pd;

    // change qp to init
    if (change_qpstate_to_init(rdma_res, &config) != SUCCESS) {
        fprintf(stderr, "Failed to change QP state to INIT\n");
        goto cleanup;
    }

    // create connection into
    local_info = create_connection_info(rdma_res, ctx);
    remote_info = malloc(sizeof(*remote_info));
    if (!remote_info || !local_info) {
        fprintf(stderr, "Failed to create connection info\n");
        goto cleanup;
    }

    if(exchange_connection_info(local_info, remote_info) != SUCCESS) {
        fprintf(stderr, "Failed to exchange connection info\n");
        goto cleanup;
    }

    // save remote buf info 
    ctx->remote_addr = remote_info->addr;
    ctx->remote_rkey = remote_info->rkey;


    // change qp state to RTR 
    if (change_qp_to_RTR(rdma_res, remote_info, &config) != SUCCESS) {
        fprintf(stderr, "Failed to change QP state to RTR\n");
        goto cleanup;
    }

    // change qp state to rts
    if (change_qp_to_RTS(rdma_res) != SUCCESS) {
        fprintf(stderr, "Failed to change QP state to RTS\n");
        goto cleanup;
    }

    // run server
    run_server(ctx, user_param->queue_depth, buf_size);
    ret = 0;

    // TODO: Handle Multithread
    // TODO: Register Memory 

cleanup:
    if (ctx) cleanup_server_context(ctx, user_param ? user_param->queue_depth : 0);
    if (rdma_res) rdma_free_resources(rdma_res);
    if (local_info) free(local_info);
    if (remote_info) free(remote_info);
    if (user_param) free(user_param);
    return ret;
}


/////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////


// server initialization
struct server_context *setup_server(struct rdma_resources *res, int queue_depth, int buf_size) {
    struct server_context *ctx = malloc(sizeof(struct server_context));
    if (!ctx) {
        perror("Failed to allocate memory for server context");
        return NULL;
    }

    void *buffer_region = malloc(buf_size * queue_depth);
    if (!buffer_region) {
        perror("Failed to allocate memory for buffers");
        free(ctx);
        return NULL;
    }

    // initialize buffers
    bool failed = false;
    for (int i = 0; i < queue_depth; ++i) {
        ctx->buffers[i].buffer = (char *) buffer_region + (buf_size * i);
        ctx->buffers[i].mr = ibv_reg_mr(res->pd, ctx->buffers[i].buffer, buf_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        if (!ctx->buffers[i].mr) {
            perror("Failed to register memory for buffer");
            failed = true;
            break;
        }
        ctx->buffers[i].in_use = false;
    }

    if(failed) {
        // Cleanup already registered MRs
        for (int i = 0; i < queue_depth; i++) {
            if (ctx->buffers[i].mr) {
                ibv_dereg_mr(ctx->buffers[i].mr);
            }
        }
        free(buffer_region);
        free(ctx);
        return NULL;
    }


    ctx->page_id_send = malloc(sizeof(uint32_t *));  // Just one uint32_t
    if (!ctx->page_id_send) {
        perror("Failed to allocate memory for page_id_send");
        cleanup_server_context(ctx, queue_depth);
        return NULL;
    }

    ctx->page_id_mr = ibv_reg_mr(res->pd, ctx->page_id_send, sizeof(uint32_t *),
                            IBV_ACCESS_LOCAL_WRITE);
                            
    if (!ctx->page_id_mr) {
        perror("Failed to register page_id_mr");
        cleanup_server_context(ctx, queue_depth);
        return NULL;
    }

    ctx->next_page_id = 0; // TODO: Modify based on compression logic
    return ctx;
}

// main server loop
void run_server(struct server_context *ctx, int queue_depth, int buf_size) {
    // post initial receives for all buffers 
    for (int i = 0; i < queue_depth; ++i) {
        post_receive_for_buffer(ctx, i, buf_size);
    }
    
    while(1) {
        struct ibv_wc wc;
        int ne = ibv_poll_cq(ctx->cq, 1, &wc);
        if (ne < 0) {
            // handle error
            continue;
        }
        if (ne == 0) continue;

        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Completion error: %s\n", 
                        ibv_wc_status_str(wc.status));
            continue;
        }

        if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
            // write job from client
            uint32_t buf_index = wc.imm_data;
            process_received_job(ctx, buf_index, buf_size);
        }
        else if (wc.opcode == IBV_WC_RDMA_WRITE) {
            // page_id send completed
            handle_pageid_sent(ctx, &wc, buf_size);
        }
    }
}


void post_receive_for_buffer(struct server_context *ctx, int buf_index, int buf_size) {
    struct buffer_entry *entry = &ctx->buffers[buf_index];

    struct ibv_sge sg = {
        .addr = (uint64_t)entry->buffer,
        .length = buf_size,
        .lkey = entry->mr->lkey
    };

    struct ibv_recv_wr wr = {
        .wr_id = (uint64_t)entry->buffer,
        .sg_list = &sg,
        .num_sge = 1
    };

    struct ibv_recv_wr *bad_wr;
    if (ibv_post_recv(ctx->qp, &wr, &bad_wr)) {
        fprintf(stderr, "Failed to post receive for buffer %d\n", buf_index);
    }
}

// process received job
void process_received_job(struct server_context *ctx, uint32_t buf_index, int buf_size) {
    struct buffer_entry *entry = &ctx->buffers[buf_index];
    (void) buf_size;

    if (entry->in_use) {
        fprintf(stderr, "Error: Buffer %d already in use.\n", buf_index);
        return;
    }
    entry->in_use = true;
    on_compression_complete(ctx, buf_index, NULL);
    // compress_data_async(entry->buffer, buf_size, on_compression_complete, ctx, buf_index);
}

void store_compressed_data (uint32_t page_id, compressed_result_t *result) {
    (void) page_id;
    (void) result;
    return; // TODO: Complete
}

// compression callback
void on_compression_complete(void *context, int buf_index, compressed_result_t *result) {
    struct server_context *ctx = (struct server_context *) context;
    struct buffer_entry *entry = &ctx->buffers[buf_index];
    (void) entry;

    // assign page_id 
    *ctx->page_id_send = ctx->next_page_id++; // TODO: Modify logic 
    store_compressed_data(ctx->next_page_id, result);

    // send using RDMA write imm
    struct ibv_sge sg = {
        .addr = (uint64_t)ctx->page_id_send,        // Address of the local page_id value
        .length = sizeof(uint32_t),
        .lkey = ctx->page_id_mr->lkey
    };

    struct ibv_send_wr wr = {
        .wr_id = buf_index,
        .sg_list = &sg,
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
        .send_flags = IBV_SEND_SIGNALED,
        .imm_data = (uint32_t) buf_index,
        .wr.rdma = {
            .remote_addr = ctx->remote_addr + (sizeof(uint32_t) * buf_index),
            .rkey = ctx->remote_rkey
        }
    };

    struct ibv_send_wr *bad_wr;
    if (ibv_post_send(ctx->qp, &wr, &bad_wr)){
        // TODO: handle error
    }

}

void handle_pageid_sent(struct server_context *ctx, struct ibv_wc *wc, int buf_size) {
    int buf_index = wc->wr_id;
    struct buffer_entry *entry = &ctx->buffers[buf_index];

    // mark buffer as available 
    entry->in_use = false;
    post_receive_for_buffer(ctx, buf_index, buf_size);
}

void cleanup_server_context(struct server_context *ctx, int queue_depth) {
    if (!ctx) return;

    // Free buffer region (only free the first pointer since it points to the whole region)
    if (ctx->buffers[0].buffer) {
        for (int i = 0; i < queue_depth; i++) {
            if (ctx->buffers[i].mr) {
                ibv_dereg_mr(ctx->buffers[i].mr);
            }
        }
        free(ctx->buffers[0].buffer);
    }
    if (ctx->page_id_mr) ibv_dereg_mr(ctx->page_id_mr);
    if (ctx->page_id_send) free(ctx->page_id_send);
    free(ctx);
}