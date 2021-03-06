#include <iostream>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>
#include <string.h>
#include <event.h>

using namespace std;

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 9091
#define BUF_SIZE 1024

// 用户写事件完成后销毁,在 on_write()中执行
struct sock_ev_write
{
    struct event* write_ev;
    char* buffer;
};
// 用于读事件终止(socket断开)后的销毁
struct sock_ev
{
    struct event_base* base;
    struct event* read_ev;
};

// 销毁写事件用到的结构体
void destroy_sock_ev_write(struct sock_ev_write* sock_ev_write_struct)
{
    if(NULL != sock_ev_write_struct)
    {
        if(NULL != sock_ev_write_struct->write_ev)
            free(sock_ev_write_struct->write_ev);
        if(NULL != sock_ev_write_struct->buffer)
            free(sock_ev_write_struct->buffer);

        free(sock_ev_write_struct);
    }
}

// 读事件结束后,用于销毁相应的资源
void destroy_sock_ev(struct sock_ev* sock_ev_struct)
{
    if(NULL == sock_ev_struct)
        return;

    event_del(sock_ev_struct->read_ev);
    event_base_loopexit(sock_ev_struct->base, NULL);  // stop the loop
    if(NULL != sock_ev_struct->read_ev)
        free(sock_ev_struct->read_ev);

    event_base_free(sock_ev_struct->base);
    free(sock_ev_struct);
}

int getSocket()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(-1 == fd)
        cout << "ERROR, fd is -1" << endl;
    return fd;
}

// 运行在子线程中,对数据进行修改,然后通过socket写回到client端.
void on_write(int sock, short event, void* arg)
{
    cout << "on_write() called, sock = " << sock << endl;
    if(NULL == arg)
    {
        cout << "ERROR, void * arg is NULL in on_write()" << endl;
        return;
    }

    struct sock_ev_write* sock_ev_write_struct = (struct sock_ev_write*)arg;

    char buffer[BUF_SIZE];
    //

    int write_num = write(sock, buffer, strlen(buffer));
    destroy_sock_ev_write(sock_ev_write_struct);
    cout << "on_write() finished, sock = " << sock << endl;
}

// 运行在子线程中,从 socket 缓冲区中读取数据,读完数据之后,将一个"写事件"注册到"子线程的base"上,一旦 socket 可写,就调用on_write()函数
void on_read(int sock, short event, void* arg)
{
    cout << "on_read() called, sock = " << sock << endl;
    if(NULL == arg)
        return;

    struct sock_ev* event_struct = (struct sock_ev*)arg; // 获取传进来的参数
    char* buffer = new char[BUF_SIZE];
    memset(buffer, 0, sizeof(char) * BUF_SIZE );    // 求数组占空间的大小

    // 此处本来应该用while循环, 但由于使用了 libevent, 只在可以读的时候才触发on_write(), 故不必用while循环了
    int size = read(sock, buffer, BUF_SIZE);
    if(0 == size) // 说明socket关闭
    {
        cout << "read size is 0 for socket : " << socket << endl;
        destroy_sock_ev(event_struct);
        close(sock);
        return;
    }
    struct sock_ev_write* sock_ev_write_struct = (struct sock_ev_write*)malloc(sizeof(struct sock_ev_write));
    sock_ev_write_struct->buffer = buffer;
    struct event* write_ev = (struct event*)malloc(sizeof(struct event));//发生写事件（也就是只要socket缓冲区可写）时，就将反馈数据通过socket写回客户端
    sock_ev_write_struct->write_ev = write_ev;

    event_set(write_ev, sock, EV_WRITE, on_write, sock_ev_write_struct);
    event_base_set(event_struct->base, write_ev);
    event_add(write_ev, NULL);
    cout<<"on_read() finished, sock="<<sock<<endl;
}


// main执行accept()得到新socket_fd的时候，执行这个方法.创建一个新线程，在新线程里反馈给client收到的信息
void* process_in_new_thread_when_accepted(void* arg)
{
   long long_fd = (long)arg;
   int fd = (int)long_fd;
   if(fd<0){
       cout<<"process_in_new_thread_when_accepted() quit!"<<endl;
       return 0;
   }
   //-------初始化base,写事件和读事件--------
   struct event_base* base = event_base_new();
   struct event* read_ev = (struct event*)malloc(sizeof(struct event));//发生读事件后，从socket中取出数据
   //-------将base，read_ev,write_ev封装到一个event_struct对象里，便于销毁---------
   struct sock_ev* event_struct = (struct sock_ev*)malloc(sizeof(struct sock_ev));
   event_struct->base = base;
   event_struct->read_ev = read_ev;
   //-----对读事件进行相应的设置------------
   event_set(read_ev, fd, EV_READ|EV_PERSIST, on_read, event_struct);
   event_base_set(base, read_ev);
   event_add(read_ev, NULL);
   //--------开始libevent的loop循环-----------
   event_base_dispatch(base);
   cout<<"event_base_dispatch() stopped for sock("<<fd<<")"<<" in process_in_new_thread_when_accepted()"<<endl;
   return 0;
}

// 每当accept出一个新的socket_fd时，调用这个方法。创建一个新线程，在新线程里与client做交互
void accept_new_thread(int sock){
    pthread_t thread;
    pthread_create(&thread,NULL,process_in_new_thread_when_accepted,(void*)sock);
    pthread_detach(thread);
}


// 每当有新连接连到server时，就通过libevent调用此函数。每个连接对应一个新线程

void on_accept(int sock, short event, void* arg)
{
    struct sockaddr_in remote_addr;
    int sin_size=sizeof(struct sockaddr_in);
    // 4.accept the requirement of some client
    int new_fd = accept(sock,  (struct sockaddr*) &remote_addr, (socklen_t*)&sin_size);
    if(new_fd < 0){
        cout<<"Accept error in on_accept()"<<endl;
        return;
    }
    cout<<"new_fd accepted is "<<new_fd<<endl;
    accept_new_thread(new_fd);
    cout<<"on_accept() finished for fd="<<new_fd<<endl;
}


int main()
{
    // 1. create a socket.
    int fd = getSocket();
    if(fd < 0)
        cout << "ERROR in main(), fd < 0" << endl;

    cout << "main() fd : " << fd << endl;

    // 2. bind the socket.
    struct sockaddr_in local_addr;  // 服务器网络端地址结构体
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;  // 设置为IP通信
    local_addr.sin_addr.s_addr=inet_addr(SERVER_IP);//服务器IP地址
    local_addr.sin_port=htons(SERVER_PORT); //服务器端口号
    int bind_result = bind(fd, (struct sockaddr*)&local_addr, sizeof(sockaddr));
    if(bind_result < 0)
    {
        cout << "Bind Error in main()" << endl;
        return -1;
    }
    cout << "bind result : " << bind_result << endl;

    // 3. listen the socket
    listen(fd, 10);

    // 设置libevent事件,每当socket出现可读事件,就调用on_accept()
    struct event_base* base = event_base_new();
    struct event listen_ev;
    event_set(&listen_ev, fd, EV_READ | EV_PERSIST, on_accept, NULL);
    event_base_set(base, &listen_ev);
    event_add(&listen_ev, NULL);
    event_base_dispatch(base);

     //------以下语句理论上是不会走到的---------------------------
    cout<<"event_base_dispatch() in main() finished"<<endl;
    //----销毁资源-------------
    event_del(&listen_ev);
    event_base_free(base);
    cout<<"main() finished"<<endl;

    return 0;
}


/**
event_init ： 初始化libevent库。

event_set ：赋值structevent结构。可以用event_add把该事件结构增加到事件循环，用event_del从事件循环中删除。支持的事件类型可以是下面组合：EV_READ（可读）,  EV_WRITE（可写），EV_PERSIST（除非调用event_del，否则事件一直在事件循环中）。

event_base_set ：修改structevent事件结构所属的event_base为指定的event_base。Libevnet内置一个全局的event_base结构。多个线程应用中，如果多个线程都需要一个libevent事件循环，需要调用event_base_set修改事件结构基于的event_base。

event_add ：增加事件到事件监控中。

event_base_loop ：事件循环。调用底层的select、poll或epoll等，如监听事件发生，调用事件结构中指定的回调函数。
*/

















