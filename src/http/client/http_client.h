/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef __HTTP_CLIENT_H__
#define __HTTP_CLIENT_H__

#include <boost/asio/ip/tcp.hpp>
#include <boost/function.hpp>
#include <boost/ptr_container/ptr_map.hpp>
#include <boost/system/error_code.hpp>
#include <string>
#include "base/queue_task.h"
#include "base/timer.h"
#include "io/tcp_server.h"
#include "io/tcp_session.h"

class LifetimeActor;
class LifetimeManager;
class HttpClient;
class HttpConnection;
struct _ConnInfo;
struct _GlobalInfo;

enum Event {
    EVENT_NONE,
    ACCEPT,
    CONNECT_COMPLETE,
    CONNECT_FAILED,
    CLOSE
};

typedef boost::function<void()> EnqueuedCb;
class HttpClientSession : public TcpSession {
public:

    HttpClientSession(HttpClient *client, Socket *socket);
    virtual ~HttpClientSession() { assert(delete_called_ != 0xdeadbeaf); delete_called_ = 0xdeadbeaf; }
    virtual void OnRead(Buffer buffer);

    void SetConnection(HttpConnection *conn) { connection_ = conn; }
    HttpConnection *Connection() { return connection_; }
    tbb::mutex &mutex() { return mutex_; }

private:
    void OnEvent(TcpSession *session, Event event);
    HttpConnection *connection_;
    uint32_t delete_called_;
    tbb::mutex mutex_;

    DISALLOW_COPY_AND_ASSIGN(HttpClientSession);
};

class HttpConnection {
public:
    HttpConnection(boost::asio::ip::tcp::endpoint, size_t id, HttpClient *);
    ~HttpConnection();

    int Initialize();

    typedef boost::function<void(std::string &, boost::system::error_code &)> HttpCb;
    int HttpPut(std::string &put_string, std::string &path, HttpCb);
    int HttpGet(std::string &path, HttpCb);

    struct _ConnInfo *curl_handle() { return curl_handle_; }
    HttpClient *client() { return client_; }
    HttpClientSession *session() { return session_; }
    tbb::mutex &mutex() { return mutex_; }
    boost::asio::ip::tcp::endpoint endpoint() { return endpoint_; }
    size_t id() { return id_; }

    const std::string &GetData();
    void set_curl_handle(struct _ConnInfo *handle) { curl_handle_ = handle; }
    HttpClientSession *CreateSession();
    void set_session(HttpClientSession *session);
    void AssignData(const char *ptr, size_t size);
    void UpdateOffset(size_t bytes);
    size_t GetOffset();
    HttpCb HttpClientCb() { return cb_; }

private:
    std::string make_url(std::string &path);
    void HttpPutInternal(std::string put_string, std::string path, HttpCb);
    void HttpGetInternal(std::string path, HttpCb);

    // key = endpoint_ + id_ 
    boost::asio::ip::tcp::endpoint endpoint_;
    size_t id_; 
    HttpCb cb_;
    size_t offset_;
    std::string buf_;
    struct _ConnInfo *curl_handle_;
    HttpClientSession *session_;
    HttpClient *client_;
    mutable tbb::mutex mutex_;

    DISALLOW_COPY_AND_ASSIGN(HttpConnection);
};

// Http Client class
class HttpClient : public TcpServer {
public:
    explicit HttpClient(EventManager *evm);
    virtual ~HttpClient();

    void Init();
    void Shutdown();
    void SessionShutdown(); 

    virtual TcpSession *CreateSession();
    HttpConnection *CreateConnection(boost::asio::ip::tcp::endpoint);
    bool AddConnection(HttpConnection *);
    void RemoveConnection(HttpConnection *);


    void ProcessEvent(EnqueuedCb cb);
    struct _GlobalInfo *GlobalInfo() { return gi_; }
    boost::asio::io_service *io_service();

    void StartTimer(long);
    void CancelTimer();

    bool IsErrorHard(const boost::system::error_code &ec);

protected:
    virtual TcpSession *AllocSession(Socket *socket);

private:
    void TimerErrorHandler(std::string name, std::string error); 
    void RemoveConnectionInternal(HttpConnection *);
    bool DequeueEvent(EnqueuedCb);
    void ShutdownInternal(); 

    typedef boost::asio::ip::tcp::endpoint endpoint;
    typedef std::pair<endpoint, size_t> Key;
    typedef boost::ptr_map<Key, HttpConnection> HttpConnectionMap;

    bool TimerCb();
    struct _GlobalInfo *gi_;
    Timer *curl_timer_;
    HttpConnectionMap map_;
    size_t id_;

    WorkQueue<EnqueuedCb> work_queue_;

    DISALLOW_COPY_AND_ASSIGN(HttpClient);
};

#endif
