/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef __OPSERVERPROXY_H__
#define __OPSERVERPROXY_H__

#include <string>
#include "io/event_manager.h"

// This class can be used to send UVE Traces from vizd to the OpSever(s)
// Currently, this is done via Redis. 
class VizCollector;

class OpServerProxy {
public:

    enum CmdStatus {
        STATUS_INVALID = 0,
        STATUS_ERROR = 1,
        STATUS_NOTPRESENT = 2,
        STATUS_PRESENT = 3,
    };

    // To construct this interface, pass in the hostname and port for Redis
    OpServerProxy(EventManager *evm, VizCollector *collector,
            const std::string & redis_ip,
            unsigned short redis_port,
            int gen_timeout = 0);
    OpServerProxy() : impl_(NULL) { }
    virtual ~OpServerProxy();

    virtual bool UVEUpdate(const std::string &type, const std::string &attr,
                           const std::string &source, const std::string &module,
                           const std::string &key, const std::string &message,
                           int32_t seq, const std::string& agg, 
                           const std::string& atyp, int64_t ts);

    // Use this to delete the object when the deleted attribute is set
    virtual bool UVEDelete(const std::string &type,
                       const std::string &source, const std::string &module,
                       const std::string &key, int32_t seq);

    typedef boost::function<void(const std::map<std::string,int32_t> &)> GetSeqReply;
    virtual bool GetSeq(const std::string &source, const std::string &module, GetSeqReply gsr);

    typedef boost::function<void(bool)> DeleteUVEsReply;
    virtual bool DeleteUVEs(const std::string &source, const std::string &module, DeleteUVEsReply dur);

    bool RefreshGenerator(const std::string &source, const std::string &module);
    bool WithdrawGenerator(const std::string &source, const std::string &module);
    typedef boost::function<void(int)> GenCleanupReply;
    bool GeneratorCleanup(GenCleanupReply gcr);

private:
    class OpServerImpl;
    OpServerImpl *impl_;
    int gen_timeout_;    
};
#endif
