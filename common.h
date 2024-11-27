#ifndef COMMON_H
#define COMMON_H

#include <infiniband/verbs.h>
#include <inttypes.h>
#include <stdbool.h>

/* Constants */
#define SUCCESS 0
#define FAILURE -1

/* RDMA Specific Configuration */
#define IBV_DEVICE_MAX_LENGTH 10
#define MAX_QUEUE_DEPTH 20
#define MAX_JOBS 100
#define PAGE_SIZE 4096
#define IB_PORT_DEFAULT 1

/* Memory Region Access Flags */
#define MR_LOCAL_FLAGS (IBV_ACCESS_LOCAL_WRITE)
#define MR_REMOTE_FLAGS (IBV_ACCESS_LOCAL_WRITE | \
                        IBV_ACCESS_REMOTE_READ | \
                        IBV_ACCESS_REMOTE_WRITE)

// connection info exchanged between client and server
typedef struct connection_info {
    union ibv_gid gid;
    int qpn;        // queue pair number
    uint64_t addr;  // buffer address
    uint32_t rkey;  // remote key
} connection_info;

typedef struct rdma_resources {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    union ibv_gid gid;
    uint32_t qp_num;    // qp num 
} rdma_resources;

// Configuration parameters
typedef struct rdma_config {
    char *ib_devname;
    int ib_port;
    int cq_size;
    int num_qp_wr;            // Number of Work Requests per QP
    int num_sge;              // Number of Scatter/Gather Elements
    bool use_event;           // Use event-driven completions
} rdma_config;

int validate_ip(const char *ip_str);

struct rdma_resources* rdma_init_resources(struct rdma_config *config);

void rdma_free_resources(struct rdma_resources *res);

struct ibv_context* create_context(char **ib_devname);

uint16_t get_local_id(struct ibv_context* context, int ib_port);

uint32_t get_qp_num(struct ibv_qp* qp);

int change_qpstate_to_init(struct rdma_resources *res, struct rdma_config *config);

int change_qp_to_RTR(struct rdma_resources *lres, struct connection_info *rres, struct rdma_config *config);

int change_qp_to_RTS(struct rdma_resources *res);


#endif /* COMMON_H */