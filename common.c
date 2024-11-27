#include "common.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>

// Function to validate IP address
int validate_ip(const char *ip_str) {
    struct sockaddr_in addr;
    if (inet_pton(AF_INET, ip_str, &(addr.sin_addr)) != 1) {
        fprintf(stderr, "Invalid IP address format: %s\n", ip_str);
        return FAILURE;
    }
    return SUCCESS;
}

struct rdma_resources* rdma_init_resources(struct rdma_config *config) {
    struct rdma_resources *res = calloc(1, sizeof(*res));
    if (!res) return NULL;

    // get device context
    res->context = create_context(&config->ib_devname);
    if (!res->context) goto cleanup;

    // create pd 
    res->pd = ibv_alloc_pd(res->context);
    if (!res->pd) goto cleanup;

    // create CQ
    res->cq = ibv_create_cq(res->context, config->cq_size, NULL, NULL, 0);
    if (!res->cq) goto cleanup;

    struct ibv_qp_init_attr qp_init_attr = {
        .qp_type = IBV_QPT_RC,
        .sq_sig_all = 1,
        .send_cq = res->cq,
        .recv_cq = res->cq,
        .cap = {
            .max_send_wr = config->num_qp_wr,
            .max_recv_wr = config->num_qp_wr,
            .max_send_sge = config->num_sge,
            .max_recv_sge = config->num_sge
        }
    };
    res->qp = ibv_create_qp(res->pd, &qp_init_attr);
    if (!res->qp) goto cleanup;

    // get local ID and QP number
    res->lid = get_local_id(res->context, config->ib_port);
    res->qp_num = get_qp_num(res->qp);

    return res;

cleanup:
    rdma_free_resources(res);
    return NULL;
}

void rdma_free_resources(struct rdma_resources *res) {
    if (!res) return;

    if (res->qp) ibv_destroy_qp(res->qp);
    if (res->cq) ibv_destroy_cq(res->cq);
    if (res->pd) ibv_dealloc_pd(res->pd);
    if (res->context) ibv_close_device(res->context); 
    free(res);
}

// find ibv dev, return ibv_context
struct ibv_context* create_context(char **ib_devname)
{
    struct ibv_context* context = NULL;
	int num_devices;
	struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
	struct ibv_device *ib_dev = NULL;

    for (; (ib_dev = *dev_list); ++dev_list) {
        if (!strcmp(ibv_get_device_name(ib_dev), *ib_devname)) {
            context = ibv_open_device(ib_dev);
            break;
        }
    }
    
    ibv_free_device_list(dev_list);
    if (context == NULL) {
        fprintf(stderr, "Unable to find/open device\n");
        return NULL;
    }
    return context;
}

// Function to get local LID
uint16_t get_local_id(struct ibv_context* context, int ib_port) {
    struct ibv_port_attr port_attr;
    if (ibv_query_port(context, ib_port, &port_attr)) {
        fprintf(stderr, "Failed to query port %d\n", ib_port);
        return 0;
    }
    return port_attr.lid;
}

// Function to get QP number
uint32_t get_qp_num(struct ibv_qp* qp) {
    return qp->qp_num;
}

// Function to modify QP state to INIT
int change_qpstate_to_init(struct rdma_resources *res, struct rdma_config *config) {
    struct ibv_qp_attr init_attr;
    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.qp_state = IBV_QPS_INIT;
    init_attr.port_num = config->ib_port;
    init_attr.pkey_index = 0;
    init_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    if (ibv_modify_qp(res->qp, &init_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        return FAILURE;
    }
    return SUCCESS;
}

// Function to modify QP state to Ready to Receive (RTR)
int change_qp_to_RTR(struct rdma_resources *lres, struct connection_info *rres, struct rdma_config *config) {
    struct ibv_qp_attr rtr_attr;
    memset(&rtr_attr, 0, sizeof(rtr_attr));

    rtr_attr.qp_state = IBV_QPS_RTR;
    rtr_attr.path_mtu = IBV_MTU_1024;
    rtr_attr.rq_psn = 0;
    rtr_attr.max_dest_rd_atomic = 1;
    rtr_attr.min_rnr_timer = 0x12;
    rtr_attr.ah_attr.is_global = 0;
    rtr_attr.ah_attr.sl = 0;
    rtr_attr.ah_attr.src_path_bits = 0;
    rtr_attr.ah_attr.port_num = config->ib_port;
    
    rtr_attr.dest_qp_num = rres->qpn;
    rtr_attr.ah_attr.dlid = rres->lid;

    printf("remote: %d local: %d\n", rres->qpn, lres->qp_num);
    if (ibv_modify_qp(lres->qp, &rtr_attr,
            IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return FAILURE;
    }
    return SUCCESS;
}

// Function to modify QP state to Ready to Send (RTS)
int change_qp_to_RTS(struct rdma_resources *res) {
    struct ibv_qp_attr rts_attr;
    memset(&rts_attr, 0, sizeof(rts_attr));
    rts_attr.qp_state = IBV_QPS_RTS;
    rts_attr.timeout = 0x12;
    rts_attr.retry_cnt = 7;
    rts_attr.rnr_retry = 7;
    rts_attr.sq_psn = 0;
    rts_attr.max_rd_atomic = 1;

    if (ibv_modify_qp(res->qp, &rts_attr,
            IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN )) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return FAILURE;
    }
    return SUCCESS;
}

