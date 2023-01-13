//
// Created by 梁隽宁 on 2022/11/24.
//
#include "http_conn.h"
#include "log.h"
#include <map>
#include <mysql/mysql.h>
#include <fstream>

//#define connfdET //边缘触发非阻塞
#define connfdLT //水平触发阻塞

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

//当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
const char *doc_root = "/home/ljn/TinyWebServer/root";//网站的根目录，之后要改

//将数据库中的用户名和密码载入到服务器的map中来
//将表中的用户名和密码放入map
map<string, string> users;
locker m_lock;

void http_conn::initmysql_result(connection_pool *connPool)//初始化数据库结果集
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);//这是个类

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))//使用mysql_query()执行查询语句
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    //使用result = mysql_store_result(mysql)获取结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    //使用mysql_num_fields(result)获取查询的列数
    //mysql_num_rows(result)获取结果集的行数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))//通过mysql_fetch_row(result)不断获取下一行，然后循环输出
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET//ET边缘触发模式
    /*epoll_wait检测到文件描述符有事件发生，则将其通知给应用程序，应用程序必须立即处理该事件
     * 必须要一次性将数据读取完，使用非阻塞I/O，读取到出现eagain*/
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;//EPOLLRDHUP事件，代表对端断开连接
#endif

#ifdef connfdLT//LT水平触发模式
    /*epoll_wait检测到文件描述符有事件发生，则将其通知给应用程序，应用程序可以不立即处理该事件
     * 当下一次调用epoll_wait时，epoll_wait还会再次向应用程序报告此事件，直至被处理*/
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

#ifdef listenfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef listenfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //epoll_ctl()中的参数event表示要检测文件描述符的什么事情，即检测读事件还是写事件
    //设置文件描述符非阻塞
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev)
{
    //修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    //int reuse=1;
    //setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    //添加到epoll对象中
    addfd(m_epollfd, sockfd, true);
    m_user_count++;//总用户数+1
    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;//初始化状态为解析请求首行
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    //memset是一个初始化函数，作用是将某一块内存中的全部设置为指定的值
    //s指向要填充的内存块，c是要被设置的值，n是要被设置该值的字符数，返回类型是一个指向存储区s的指针
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容，解析一行，判断依据\r\n
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)//就代表没有读取到\n
                return LINE_OPEN;//没有读取完整
            else if (m_read_buf[m_checked_idx + 1] == '\n')//证明是\r\n
            {
                m_read_buf[m_checked_idx++] = '\0';//把\r\n转换为字符串结束符
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')//读到了\n，看看前面是不是\r
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)//缓冲区已满
    {
        return false;
    }
    //已经读取到的字节
    int bytes_read = 0;

#ifdef connfdLT//不需要一次性读完。所以没有循环
    //第二个参数buf和第三个参数len分别指定读缓冲区的位置和大小，flag通常设置为0
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
    m_read_idx += bytes_read;

    if (bytes_read <= 0)
    {
        return false;
    }

    return true;

#endif

#ifdef connfdET//要一次性读完，所以用循环
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1)
        {
            //EAGAIN通常在执行非阻塞I/O时引发.这意味着目前没有可用数据，请稍后再试
            //EWOULDBLOCK表示发送时套接字发送缓冲区已满，或接收时套接字接收缓冲区为空
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;//没有数据了
            return false;
        }
        else if (bytes_read == 0)
        {
            return false;//对方关闭连接
        }
        m_read_idx += bytes_read;
    }
    return true;
#endif
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //GET /index.html HTTP/1.1
    //strpbrk是在源字符串（s1）中找出最先含有搜索字符串（s2）中任一字符的位置并返回，若找不到则返回空指针
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)//C语言中判断字符串是否相等的函数，忽略大小写
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串str2中出现的字符下标
    //m_url+= strspn(m_url," \t");//strspn()函数返回一个数字，m_url作为指针会自增
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    /*有的请求是这样的
     * http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        //192.168.110.129:10000/index.html
        m_url = strchr(m_url, '/');// /index.html
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;//主状态机的状态变成检查请求头
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //遇到空行，表示头部字段解析完毕
    if (text[0] == '\0')
    {
        //如果http请求有消息体，则还需要读取m_content_length字节的信息体
        //状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则说明我们已经得到了一个完整的http请求
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        //处理Connection头部字段 Connection：keep-alive
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        //处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        //处理Host头部字段
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        //printf("oop!unknow header: %s\n",text);
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    return NO_REQUEST;
}

//我们没有真正解析http请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//主状态机解析请求
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;//该变量为从状态机的状态，一开始先设定状态
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;//要获取的数据

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        //解析到了一行完整的数据，或者解析到了请求体（也是完整的数据）
        //获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        Log::get_instance()->flush();
        switch (m_check_state)//主状态机
        {
            case CHECK_STATE_REQUESTLINE://分析请求行
            {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER://分析头部字段
            {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                else if (ret == GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT://解析请求体
            {
                ret = parse_content(text);
                if (ret == GET_REQUEST)
                    return do_request();//成功
                line_status = LINE_OPEN;//改变从状态
                break;
            }
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    //处理响应
    //"/home/nowcoder/webserver/resources"
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        //以&为分隔符，前面的为用户名
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        //以&为分隔符，后面的是密码
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        //同步线程登录校验
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);//strcat函数又被称为是字符串追加/连接函数，它的功能就是在一个字符串后面追加上另外一个字符串
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            //判断map中能否找到重复的用户名
            if (users.find(name) == users.end())
            {
                //向数据库中插入数据时，需要通过锁来同步数据
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)//校验成功，跳转登录页面
                    strcpy(m_url, "/log.html");
                else//校验失败，跳转注册失败页面
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
            //如果是登录，直接判断
            //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')//如果请求资源为/0，表示跳转注册界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')//如果请求资源为/1，表示跳转登录界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        //将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')//图片页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')//视频页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')//关注页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else//否则发送url实际请求的文件
    {
        //如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
        //这里的情况是welcome界面，请求服务器上的一个图片
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
        //这个过后m_real_file里就保存了/home/nowcoder/webserver/resources/index.html，m_url是/index.html
        //获取m_real_file文件的相关的状态信息，-1失败，0成功
    }
    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    if (!(m_file_stat.st_mode & S_IROTH))//判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
        return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode))//判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
        return BAD_REQUEST;
    int fd = open(m_real_file, O_RDONLY);//以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    //创建内存映射
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;//表示请求文件存在，且可以访问
}
//对内存映射区执行munmap操作
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        //mmap函数用于申请一段内存空间，用这段内存作为进程间通信的共享内存，也可以将文件直接映射到其中
        //munmap函数则释放mmap创建这段内存，start参数允许用户使用某个特定的地址作为这段内存的起始地址
        //如果它被设置为NULL则系统自动分配一个地址，length参数指定内存段的长度
        m_file_address = 0;
    }
}

bool http_conn::write()//写http响应
{
    int temp = 0;

    //若要发送的数据长度为0
    //表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        // 将要发送的字节为0，这一次响应结束。
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();//重新初始化
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);//将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        //writev在成功时返回写入fd的字节数
        if (temp < 0)
        {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if (errno == EAGAIN)//EAGAIN:要求再次尝试
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)//iov_len:这块内存的长度
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            //iov_base:内存起始地址
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();// 没有数据要发送了
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
// 往写缓冲中写入待发送的数据
bool http_conn::add_response(const char *format, ...)
{
    //如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;//代表写满了
    va_list arg_list;//定义该类型变量来指向可变参数的地址
    va_start(arg_list, format);//将变量arg_list初始化为传入参数
    //将数据format从可变参数列表写入缓冲区，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    //更新m_write_idx位置
    m_write_idx += len;
    //清空可变参列表
    va_end(arg_list);
    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}
//添加响应状态行，即响应首行：http/1.1 状态码 状态消息
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)//添加消息报头，内部调用add_content_length和add_linger函数
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();//添加空行
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()//添加连接状态，通知浏览器端是保持连接还是关闭
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)//添加文本content
{
    return add_response("%s", content);
}
// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
        case INTERNAL_ERROR:
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }
        case BAD_REQUEST:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }
            //文件存在，200
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            //如果请求的资源存在
            if (m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);
                //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                //发送的全部数据为响应报文头部信息和文件大小
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else
            {
                //如果请求的资源大小为0，则返回空白html文件
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
        default:
            return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();//解析http请求
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);//生成响应
    if (!write_ret)//如果没有响应成功
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}