/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef ctrlplane_ksync_sock_h 
#define ctrlplane_ksync_sock_h 

#include <vector>
#include <linux/rtnetlink.h>
#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/netlink_protocol.hpp>
#include <boost/asio/netlink_endpoint.hpp>
#include <tbb/atomic.h>
#include <tbb/mutex.h>
#include <base/queue_task.h>
#include <sandesh/sandesh_types.h>
#include <sandesh/sandesh.h>
#include "vr_types.h"

class KSyncEntry;

//abstract socket 
//[kernel netlink, userspace udp, userspace memorymap]
class KSyncSockType {
public:
    KSyncSockType() { };
    virtual ~KSyncSockType() { };

    typedef boost::function<void(const boost::system::error_code &, size_t)> HandlerCb;
    virtual void AsyncReceive(boost::asio::mutable_buffers_1, HandlerCb) = 0;
    virtual void AsyncSendTo(boost::asio::mutable_buffers_1, HandlerCb) = 0;
    virtual void SendTo(boost::asio::const_buffers_1) = 0;
    virtual void Receive(boost::asio::mutable_buffers_1) = 0;
private:
};

//netlink socket class for interacting with kernel
class KSyncSockTypeNetlink : public KSyncSockType {
public:
    KSyncSockTypeNetlink(boost::asio::io_service &ios, int protocol);
    virtual ~KSyncSockTypeNetlink() { };

    virtual void AsyncReceive(boost::asio::mutable_buffers_1, HandlerCb);
    virtual void AsyncSendTo(boost::asio::mutable_buffers_1, HandlerCb);
    virtual void SendTo(boost::asio::const_buffers_1);
    virtual void Receive(boost::asio::mutable_buffers_1);
private:
    boost::asio::netlink::raw::socket sock_;
};

/* Base class to hold sandesh context information which is passed to 
 * Sandesh decode
 */
class AgentSandeshContext : public SandeshContext {
public:
    AgentSandeshContext() : errno_(0) { };
    virtual ~AgentSandeshContext() { };

    virtual void IfMsgHandler(vr_interface_req *req) = 0;
    virtual void NHMsgHandler(vr_nexthop_req *req) = 0;
    virtual void RouteMsgHandler(vr_route_req *req) = 0;
    virtual void MplsMsgHandler(vr_mpls_req *req) = 0;
    virtual int VrResponseMsgHandler(vr_response *r) = 0;
    virtual void MirrorMsgHandler(vr_mirror_req *req) = 0;
    virtual void FlowMsgHandler(vr_flow_req *req) = 0;
    virtual void VrfAssignMsgHandler(vr_vrf_assign_req *req) = 0;
    virtual void VrfStatsMsgHandler(vr_vrf_stats_req *req) = 0;
    virtual void DropStatsMsgHandler(vr_drop_stats_req *req) = 0;

    void SetErrno(int err) {errno_ = err;};
    int GetErrno() const {return errno_;};
private:
    int errno_;
};


/* Base class for context management. Used while sending and 
 * receiving data via ksync socket
 */
class  IoContext {
public:
    enum IoContextWorkQId {
        DEFAULT_Q_ID,
        UVE_Q_ID,
        MAX_WORK_QUEUES // This should always be last
    };
    static const char* io_wq_names[MAX_WORK_QUEUES];
    IoContext() : ctx_(NULL), msg_(NULL), msg_len_(0), seqno_(0) { };

    IoContext(char *msg, uint32_t len, uint32_t seq, AgentSandeshContext *ctx) 
        : ctx_(ctx), msg_(msg), msg_len_(len), seqno_(seq), 
          work_q_id_(DEFAULT_Q_ID) { };
    IoContext(char *msg, uint32_t len, uint32_t seq, AgentSandeshContext *ctx, 
              IoContextWorkQId id) : ctx_(ctx), msg_(msg), msg_len_(len), 
              seqno_(seq), work_q_id_(id) { };
    virtual ~IoContext() { 
        if (msg_ != NULL)
            free(msg_);
    };

    bool operator<(const IoContext &rhs) const {
        return seqno_ < rhs.seqno_;
    };

    void SetSeqno(uint32_t seqno) {seqno_ = seqno;};
    uint32_t GetSeqno() const {return seqno_;};

    virtual void Handler() {};
    virtual void ErrorHandler(int err) {};

    AgentSandeshContext *GetSandeshContext() { return ctx_; }
    IoContextWorkQId GetWorkQId() { return work_q_id_; }
protected:
    boost::intrusive::set_member_hook<> node_;
    void UpdateNetlinkHeader();
    AgentSandeshContext *ctx_;

private:
    char *msg_;
    uint32_t msg_len_;
    uint32_t seqno_;
    IoContextWorkQId work_q_id_;

    friend class KSyncSock;
};

/* IoContext tied to KSyncEntry */
class  KSyncIoContext : public IoContext {
public:
    KSyncIoContext(KSyncEntry *sync_entry, int msg_len, char *msg,
                   uint32_t seqno, KSyncEntry::KSyncEvent event);
    virtual void Handler();
    void ErrorHandler(int err);
    const KSyncEntry *GetKSyncEntry() const {return entry_;};
private:
    KSyncEntry *entry_;
    KSyncEntry::KSyncEvent event_;
};

class KSyncSock {
public:
    const static int kMsgGrowSize = 16;
    const static unsigned kBufLen = 4096;

    typedef boost::intrusive::member_hook<IoContext,
            boost::intrusive::set_member_hook<>,
            &IoContext::node_> KSyncSockNode;
    typedef boost::intrusive::set<IoContext, KSyncSockNode> Tree;

    KSyncSock(boost::asio::io_service &ios, int protocol);
    virtual ~KSyncSock();

    // Create KSyncSock objects
    static void Init(boost::asio::io_service &ios, int count, int protocol);
    // Start Ksync Asio operations
    static void Start();
    static void Shutdown();

    // Partition to KSyncSock mapping
    static KSyncSock *Get(DBTablePartBase *partition);
    static KSyncSock *Get(int partition_id);
    // Write a KSyncEntry to kernel
    void SendAsync(KSyncEntry *entry, int msg_len, char *msg, KSyncEntry::KSyncEvent event);
    void BlockingSend(const char *msg, int msg_len);
    bool BlockingRecv();

    static uint32_t GetPid() {return pid_;};
    static int GetNetlinkFamilyId() {return vnsw_netlink_family_id_;};
    static void SetNetlinkFamilyId(int id) {vnsw_netlink_family_id_ = id;};
    int AllocSeqNo() { 
        int seq = seqno_++;
        return seq;
    }
    void GenericSend(int msg_len, char *msg, IoContext *ctx);
    static AgentSandeshContext *GetAgentSandeshContext() {
        return agent_sandesh_ctx_;
    }
    static void SetAgentSandeshContext(AgentSandeshContext *ctx) {
        agent_sandesh_ctx_ = ctx;
    }
    void Decoder(char *data, SandeshContext *ctxt);
private:
    // Read handler registered with boost::asio. Demux done based on seqno_
    void ReadHandler(const boost::system::error_code& error,
                     size_t bytes_transferred);

    // Write handler registered with boost::asio. Demux done based on seqno_
    void WriteHandler(const boost::system::error_code& error,
                      size_t bytes_transferred);

    bool ProcessKernelData(char *data);
    bool ValidateAndEnqueue(char *data);
    void SendAsyncImpl(int msg_len, char *msg, IoContext *ioc);

    static std::vector<KSyncSock *> sock_table_;
    static pid_t pid_;
    static int vnsw_netlink_family_id_;
    static AgentSandeshContext *agent_sandesh_ctx_;
    static tbb::atomic<bool> shutdown_;

    // Tree of all KSyncEntries pending ack from Netlink socket
    Tree wait_tree_;
    KSyncSockType *sock_type_;
    char *rx_buff_;
    tbb::atomic<int> seqno_;
    tbb::mutex mutex_;

    // Debug stats
    int tx_count_;
    int ack_count_;
    int err_count_;

    WorkQueue<char *> *work_queue_[IoContext::MAX_WORK_QUEUES];
    DISALLOW_COPY_AND_ASSIGN(KSyncSock);
};

#endif // ctrlplane_ksync_sock_h
