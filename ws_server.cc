
extern "C"
{
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/ws.h>
#include <evhttp.h>
}

#include <map>
#include <string>
#include <algorithm>
#include <sstream>
#include <string.h>

#define log_d(...)                                                                \
    {                                                                             \
        time_t now = time(0);                                                     \
        char timeString[64];                                                      \
        struct tm *time_info = localtime(&now);                                   \
        strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", time_info); \
        fprintf(stderr, "[%s] ", timeString);                                     \
        fprintf(stderr, __VA_ARGS__);                                             \
    }

#define reply_http_msg(req, ...)                \
    {                                           \
        evbuffer *evb = evbuffer_new();         \
        evbuffer_add_printf(evb, __VA_ARGS__);  \
        evhttp_send_reply(req, 200, "OK", evb); \
        evbuffer_free(evb);                     \
    }

class WsServer
{
public:
    WsServer() {}
    bool Init(const std::string &address, int port)
    {
        log_d("ws server init start, host=%s, port = %d\n", address.c_str(), port);
        base_ = event_base_new();
        http_server_ = evhttp_new(base_);
        evhttp_bind_socket_with_handle(http_server_, address.c_str(), port);
        evhttp_set_cb(http_server_, "/state", handleHttpState, this);
        evhttp_set_cb(http_server_, "/ws", handleWs, this);
        evhttp_set_cb(http_server_, "/push", handleHttpPush, this);
        evhttp_set_cb(http_server_, "/broadcast", handleHttpBroadCast, this);
        log_d("ws server http interface: /state,  get state of ws server\n");
        log_d("ws server http interface: /ws,  endpoint of ws connect\n");
        log_d("ws server http interface: /push, use http method push message\n");
        log_d("ws server http interface: /broadcast, use http method broadcast message\n");
        return true;
    }
    void AddSession(evutil_socket_t fd, evws_connection *session)
    {
        clients_[fd] = session;
        log_d("fd %d add to session\n", fd);
    }
    void DelSession(evutil_socket_t fd)
    {
        if (clients_.find(fd) == clients_.end())
        {
            return;
        }
        clients_.erase(fd);
        log_d("fd %d leave session\n", fd);
        if (relations_.find(fd) == relations_.end())
        {
            return;
        }
        for (auto name : relations_[fd])
        {
            LeaveRoom(name, fd);
        }
        relations_.erase(fd);
        log_d("relations erase, fd=%d\n", fd);
    }
    void JoinRoom(const std::string &name, evutil_socket_t fd)
    {
        auto &ref_room = rooms_[name];
        auto iter_room = std::find(ref_room.begin(), ref_room.end(), fd);
        if (iter_room == ref_room.end())
        {
            ref_room.push_back(fd);
            log_d("fd %d join room, name=%s\n", fd, name.c_str());
        }
        auto &ref_relation = relations_[fd];
        auto iter_relation = std::find(ref_relation.begin(), ref_relation.end(), name);
        if (iter_relation == ref_relation.end())
        {
            ref_relation.push_back(name);
            log_d("fd %d join relation, name=%s\n", fd, name.c_str());
        }
    }
    void LeaveRoom(const std::string &name, evutil_socket_t fd)
    {
        if (rooms_.find(name) == rooms_.end())
        {
            return;
        }
        auto &ref_room = rooms_[name];
        auto iter_room = std::find(ref_room.begin(), ref_room.end(), fd);
        if (iter_room != ref_room.end())
        {
            ref_room.erase(iter_room);
            log_d("fd %d leave room, name=%s\n", fd, name.c_str());
        }
        if (ref_room.size() == 0)
        {
            rooms_.erase(name);
            log_d("room clean, name=%s\n", name.c_str());
        }
        if (relations_.find(fd) == relations_.end())
        {
            return;
        }
        auto &ref_relation = relations_[fd];
        auto iter_relation = std::find(ref_relation.begin(), ref_relation.end(), name);
        if (iter_relation != ref_relation.end())
        {
            ref_relation.erase(iter_relation);
            log_d("fd %d leave relation, name=%s\n", fd, name.c_str());
        }
    }

    void Serve()
    {
        log_d("ws server start success\n");
        event_base_dispatch(base_);
    }
    void Exit()
    {
        evhttp_free(http_server_);
        event_base_free(base_);
    }

    std::string DumpState()
    {
        std::stringstream ss;
        ss << "{\"clients\":[";
        for (auto client : clients_)
        {
            ss << client.first << ",";
        }
        if (clients_.size() > 0)
        {
            ss.seekp(-1, ss.cur);
        }
        ss << "], \"rooms\":{";
        for (auto room : rooms_)
        {
            ss << "\"" << room.first << "\":[";
            for (auto name : room.second)
            {
                ss << "\"" << name << "\",";
            }
            if (room.second.size() > 0)
            {
                ss.seekp(-1, ss.cur);
            }
            ss << "],";
        }
        if (rooms_.size() > 0)
        {
            ss.seekp(-1, ss.cur);
        }
        ss << "}, \"relation\":{";
        for (auto relation : relations_)
        {
            ss << "\"" << relation.first << "\":[";
            for (auto name : relation.second)
            {
                ss << "\"" << name << "\",";
            }
            if (relation.second.size() > 0)
            {
                ss.seekp(-1, ss.cur);
            }
            ss << "],";
        }
        if (relations_.size() > 0)
        {
            ss.seekp(-1, ss.cur);
        }
        ss << "}}";
        return ss.str();
    }

    void Push(evutil_socket_t fd, const std::string &msg)
    {
        if (clients_.find(fd) == clients_.end())
        {
            return;
        }
        evws_send(clients_[fd], msg.c_str(), msg.size());
        log_d("push message,fd=%d,message====>%s\n", fd, msg.c_str());
    }

    void BroadCast(const std::string &channel, const std::string &msg)
    {
        if (rooms_.find(channel) == rooms_.end())
        {
            return;
        }
        for (auto it : rooms_[channel])
        {
            Push(it, msg);
        }
    }

private:
    static void handleHttpBroadCast(struct evhttp_request *req, void *arg)
    {
        if (evhttp_request_get_command(req) != EVHTTP_REQ_POST)
        {
            reply_http_msg(req, "{\"code\": -1, \"message\": \"invalid requst\"}");
            return;
        }
        struct evbuffer *buf = evhttp_request_get_input_buffer(req);

        char *postData = (char *)evbuffer_pullup(req->input_buffer, -1);
        struct evkeyvalq params;
        evhttp_parse_query_str(postData, &params);
        log_d("recv broadcast http message, message====>%s\n", postData);
        const char *channel = evhttp_find_header(&params, "channel");
        if (channel == NULL)
        {
            reply_http_msg(req, "{\"code\": -1, \"message\": \"params channel require\"}");
            return;
        }
        const char *message = evhttp_find_header(&params, "message");
        if (message == NULL)
        {
            reply_http_msg(req, "{\"code\": -1, \"message\": \"params message require\"}");
            return;
        }
        WsServer *server = static_cast<WsServer *>(arg);
        server->BroadCast(channel, message);

        reply_http_msg(req, "{\"code\": 0, \"message\": \"ok\"}");
    }

    static void handleHttpState(struct evhttp_request *req, void *arg)
    {
        log_d("recv state http message,\n");
        WsServer *server = static_cast<WsServer *>(arg);
        evbuffer *evb = evbuffer_new();
        std::string state = server->DumpState();
        reply_http_msg(req, "%s", state.c_str());
    }

    static void handleHttpPush(struct evhttp_request *req, void *arg)
    {
        if (evhttp_request_get_command(req) != EVHTTP_REQ_POST)
        {
            reply_http_msg(req, "{\"code\": -1, \"message\": \"invalid requst\"}");
            return;
        }
        struct evbuffer *buf = evhttp_request_get_input_buffer(req);

        char *postData = (char *)evbuffer_pullup(req->input_buffer, -1);
        struct evkeyvalq params;
        evhttp_parse_query_str(postData, &params);
        log_d("recv push http message, message====>%s\n", postData);
        const char *fd = evhttp_find_header(&params, "fd");
        if (fd == NULL)
        {
            reply_http_msg(req, "{\"code\": -1, \"message\": \"params fd require\"}");
            return;
        }
        const char *message = evhttp_find_header(&params, "message");
        if (message == NULL)
        {
            reply_http_msg(req, "{\"code\": -1, \"message\": \"params message require\"}");
            return;
        }
        WsServer *server = static_cast<WsServer *>(arg);
        server->Push(atoi(fd), message);

        reply_http_msg(req, "{\"code\": 0, \"message\": \"ok\"}");
    }
    static void handleWsMessage(struct evws_connection *evws, int type, const unsigned char *data,
                                size_t len, void *arg)
    {
        WsServer *server = static_cast<WsServer *>(arg);
        struct evkeyvalq params;
        std::string rawData((char *)data, len);
        log_d("recv ws message, message====>%s\n", rawData.c_str());
        evhttp_parse_query_str((char *)rawData.c_str(), &params);
        const char *method = evhttp_find_header(&params, "event");
        const char *sseq = evhttp_find_header(&params, "seq");
        const char *seq = sseq == NULL ? "0" : sseq;
        char buffer[1024] = {0};
        if (method == NULL)
        {
            snprintf(buffer, sizeof(buffer), "{\"event\": \"error\", \"code\": -1, \"message\": \"miss event\", id: \"%s\"}", seq);
            evws_send(evws, buffer, strlen(buffer));
            return;
        }
        if (memcmp(method, "ping", 4) == 0)
        {
            snprintf(buffer, sizeof(buffer), "{\"event\": \"pong\", \"code\": 0, \"message\": \"\", id: \"%s\"}", seq);
            evws_send(evws, buffer, strlen(buffer));
            return;
        }
        const char *channel = evhttp_find_header(&params, "channel");
        if (channel == NULL)
        {
            snprintf(buffer, sizeof(buffer), "{\"event\": \"error\", \"code\": -1, \"message\": \"miss channel\", id: \"%s\"}", seq);
            evws_send(evws, buffer, strlen(buffer));
            return;
        }
        if (memcmp(method, "sub", 3) == 0)
        {
            evutil_socket_t fd = bufferevent_getfd(evws_connection_get_bufferevent(evws));
            server->JoinRoom(channel, fd);
            snprintf(buffer, sizeof(buffer), "{\"event\": \"sub\", \"channel\": \"%s\", \"code\": 0, \"message\": \"\", id: \"%s\"}", channel, seq);
            evws_send(evws, buffer, strlen(buffer));
            return;
        }
        if (memcmp(method, "unsub", 5) == 0)
        {
            evutil_socket_t fd = bufferevent_getfd(evws_connection_get_bufferevent(evws));
            server->LeaveRoom(channel, fd);
            snprintf(buffer, sizeof(buffer), "{\"event\": \"unsub\", \"channel\": \"%s\", \"code\": 0, \"message\": \"\", id: \"%s\"}", channel, seq);
            evws_send(evws, buffer, strlen(buffer));
            return;
        }
    }

    static void handleWs(struct evhttp_request *req, void *arg)
    {
        evws_connection *session = evws_new_session(req, handleWsMessage, arg, 0);
        if (!session)
        {
            log_d("Failed to create session\n");
            return;
        }
        evutil_socket_t fd = bufferevent_getfd(evws_connection_get_bufferevent(session));
        log_d("session open, fd =%d\n", fd);
        WsServer *server = static_cast<WsServer *>(arg);
        server->AddSession(fd, session);
        evws_connection_set_closecb(session, handleClose, arg);
    }
    static void handleClose(struct evws_connection *evws, void *arg)
    {
        evutil_socket_t fd = bufferevent_getfd(evws_connection_get_bufferevent(evws));

        log_d("session close, fd =%d\n", fd);
        WsServer *server = static_cast<WsServer *>(arg);
        server->DelSession(fd);
    }

private:
    struct event_base *base_;
    struct evhttp *http_server_;
    std::map<evutil_socket_t, struct evws_connection *> clients_;
    std::map<std::string, std::vector<evutil_socket_t>> rooms_;
    std::map<evutil_socket_t, std::vector<std::string>> relations_;
};

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: ./ws_server host port");
        return 1;
    }
    WsServer *server = new WsServer();
    std::string host = argv[1];
    int port = atoi(argv[2]);
    server->Init(host, port);
    server->Serve();
}
