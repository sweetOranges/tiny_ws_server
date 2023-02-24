# tiny_ws_server
一个基于libevent实现的小巧的websocket server, 适合当ws微服务

# 用法

```
./ws_server 127.0.0.1 8080
```
# 编译
```
g++ -std=c++11 -o ws_server ws_server.cpp -I./include -L./lib -l:libevent.a
```

# 特性
- 实现了http接口 /push 按照fd推送消息
- 实现了http接口 /broadcast 广播消息
- 实现了http接口 /state 查看内部状态
