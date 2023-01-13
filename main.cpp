#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
//标准库文件 和 自定义文件 最好分开写
#include "locker.h"
#include "threadpool.h"
#include "lst_timer.h"
#include "http_conn.h"
#include "log.h"
#include "sql_connection_pool.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位

#define SYNLOG  //同步写日志
//#define ASYNLOG //异步写日志

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;//创建定时器容器链表
static int epollfd = 0;

//信号处理函数
void sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;
    //将信号值从管道写端写入，传输字符类型，而非整型
    send(pipefd[1], (char *)&msg, 1, 0);
    //将原来的errno赋值为当前的errno
    errno = save_errno;
}

//添加信号的函数
void addsig(int sig, void(handler)(int), bool restart = true)//void(handler)(int)，回调函数
{
    struct sigaction sa;//sigaction类型用来描述对信号的处理方式
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;//sa_handler指定信号处理函数，信号处理函数中仅仅发送信号值，不做对应逻辑处理
    if (restart)
        sa.sa_flags |= SA_RESTART;//sa_flags用于设置程序收到信号时的行为
    sigfillset(&sa.sa_mask);//sa_mask为信号集，指定一组信号，将所有信号添加到信号集中
    //sigfillset()在信号集中设置所有信号
    assert(sigaction(sig, &sa, NULL) != -1);
    //assert将通过检查表达式expression的值来决定是否需要终止执行程序
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick();//定时任务处理函数
    alarm(TIMESLOT);
}

//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data)
{
    //删除非活动连接在socket上的注册事件
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    //关闭文件描述符
    close(user_data->sockfd);
    http_conn::m_user_count--;//减少连接数
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}

void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])//char *argv[]:argv是一个指针数组，它的元素个数是argc，存放的是指向每一个参数的指针
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif

    if (argc <= 1)
    {
        printf("usage: %s ip_address port_number....\n", basename(argv[0]));//basename函数返回文件名
        printf("error\n");
        return 1;
    }
    printf("begin,please wait");
    //获取端口号
    int port = atoi(argv[1]);//atoi函数：转换为整数
    //对SIGPIPE信号进行处理
    addsig(SIGPIPE, SIG_IGN);//SIG_IGN信号处理函数，回调函数，表示忽略目标信号

    //创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("172.16.143.131", "root", "qwer789456123", "LNdb", 3306, 8);

    //创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...)
    {
        return 1;
    }
    //创建一个数组用于保存所有的客户端信息
    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    //初始化数据库读取表
    users->initmysql_result(connPool);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);//创建监听套接字
    assert(listenfd >= 0);

    //struct linger tmp={1,0};
    //SO_LINGER若有数据待发送，延迟关闭
    //setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;//绑定
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);

    int flag = 1;//设置端口复用
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    ret = listen(listenfd, 5);//监听
    assert(ret >= 0);

    //创建内核事件表，多路复用，创建epoll对象，事件数组，添加
    epoll_event events[MAX_EVENT_NUMBER];//events数组为传出参数，保存了发生变化的文件描述符的信息，即记录了那些文件符发生了改变
    /*
    * struct epoll_event
    * {
    *    uint32_t events;//要检测的是什么事件
    *    epoll_data_t data;//客户端用户的信息
    * }
    * */
    epollfd = epoll_create(5);//创建epoll实例，返回epoll实例对应的文件描述符
    //参数max_size标识这个监听的数目最大有多大，从Linux 2.6.8开始，max_size参数将被忽略，但必须大于零,即这个5没有实际意义
    //epoll_ctl()中的参数event表示要检测文件描述符的什么事情，即检测读事件还是写事件
    assert(epollfd != -1);
    //将监听文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    //创建管道套接字
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    //设置管道写端为非阻塞
    setnonblocking(pipefd[1]);
    //设置管道读端为ET非阻塞
    addfd(epollfd, pipefd[0], false);
    //传递给主循环的信号值，这里只关注SIGALRM和SIGTERM
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    //循环条件
    bool stop_server = false;
    //创建连接资源数组
    client_data *users_timer = new client_data[MAX_FD];
    //超时标志，超时默认为False
    bool timeout = false;
    //每隔TIMESLOT时间触发SIGALRM信号
    alarm(TIMESLOT);

    while (!stop_server)
    {
        //监测发生事件的文件描述符
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }
        //轮询文件描述符
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == listenfd)
            {
                //初始化客户端连接地址
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
                //该连接分配的文件描述符
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
                users[connfd].init(connfd, client_address);

                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                //初始化该连接对应的连接资源
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                //创建定时器临时变量
                util_timer *timer = new util_timer;
                //设置定时器对应的连接资源
                timer->user_data = &users_timer[connfd];
                //设置回调函数
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                //设置绝对超时时间
                timer->expire = cur + 3 * TIMESLOT;
                //创建该连接对应的定时器，初始化为前述临时变量
                users_timer[connfd].timer = timer;
                //将该定时器添加到链表中
                timer_lst.add_timer(timer);
#endif

#ifdef listenfdET
                while (1)
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }
                //处理异常事件
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }

                //处理定时器信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                //接收到SIGALRM信号，timeout设置为True
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                            case SIGALRM:
                            {
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }

                //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                //创建定时器临时变量，将该连接对应的定时器取出来
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once())
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    //服务器端关闭连接，移除对应的定时器
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        //处理定时器为非必须事件，收到信号并不是立马处理
        //完成读写事件后，再进行处理
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}