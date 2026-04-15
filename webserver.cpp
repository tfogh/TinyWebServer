#include "webserver.h"

WebServer::WebServer()
{
    //http_conn类对象
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;                   // 服务器监听端口
    m_user = user;                   // MySQL 用户名
    m_passWord = passWord;           // MySQL 密码
    m_databaseName = databaseName;   // MySQL 数据库名
    m_sql_num = sql_num;             // 数据库连接池大小
    m_thread_num = thread_num;       // 线程池线程数
    m_log_write = log_write;         // 日志写入方式：0 同步，1 异步
    m_OPT_LINGER = opt_linger;       // 优雅关闭连接选项
    m_TRIGMode = trigmode;           // epoll 触发模式选择
    m_close_log = close_log;         // 是否关闭日志
    m_actormodel = actor_model;      // 并发模型：0 Proactor，1 Reactor
}

void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        bool ok;
        //初始化日志
        if (1 == m_log_write)
            ok = Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            ok = Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);

        if (!ok)
        {
            // 如果日志初始化失败，关闭日志功能，避免后续写日志时使用空文件指针
            fprintf(stderr, "Warning: failed to initialize log file '%s'. Logging disabled.\n", "./ServerLog");
            m_close_log = 1;
        }
    }
}

void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化数据库读取表
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

//初始化服务器
void WebServer::eventListen()
{
    //网络编程基础步骤
    /*PF_INET 表示协议族（Protocol Family）是 IPv4 网络。也可以理解为使用 AF_INET，用于 TCP/IP IPv4。
    SOCK_STREAM 表示这是一个流式 socket。也就是 TCP 类型的 socket，面向连接、可靠传输。
    0 表示协议号由系统根据前两个参数自动选择。对于 PF_INET + SOCK_STREAM，通常会选择 IPPROTO_TCP。 */
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);//>=0代表成功创建

    //根据m_OPT_LINGER决定关闭方式
    //SO_LINGER是SOL_SOCKET的一个选项，可以通过传入linger{int,int}来控制，前者1代表开启，后者则为关闭时的等待时间，单位为秒
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address={};//储存ipv4地址的结构体
    // bzero(&address, sizeof(address));
    // memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;//设置地址族为IPv4
    address.sin_addr.s_addr = htonl(INADDR_ANY);//INADDR_ANY表示监听所有网卡上的连接，htonl将主机字节序转换为网络字节序
    address.sin_port = htons(m_port);//将端口号转换为网络字节序

    int flag = 1;
    /*设置套接字选项
        SO_REUSEADDR: 允许重用本地地址和端口，关闭后可以立刻重启服务器，从而跳过“TIME_WAIT”状态
        flag = 1 代表开启 SO_REUSEADDR 选项
    */
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    //将套接字绑定到指定的地址和端口
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    //监听套接字，等待客户端连接，允许最多 5 个未处理的连接排队。
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);
    //初始化定时器
    utils.init(TIMESLOT);

    //epoll创建内核事件表
    /*struct epoll_event
    {
        uint32_t events; epoll事件
        epoll_data_t data; 用户数据
    }*/
    epoll_event events[MAX_EVENT_NUMBER];
    //创建一个 epoll 实例的调用，m_epollfd作为创建后返回的实例标识符
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    //将文件描述符添加到 epoll 实例中，监听套接字，m_LISTENTrigmode用于设置监听套接字的触发模式
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    //创建一个双向通信的管道m_pipefd，m_pipefd[0]用于读，m_pipefd[1]用于写
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    //将文件描述符设置为非阻塞模式，确保写操作不会因为缓冲区满而阻塞
    utils.setnonblocking(m_pipefd[1]);
    //将文件描述符添加到 epoll 实例中，监听管道的读事件，触发模式为 LT
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    //为特定的信号设置处理函数
    utils.addsig(SIGPIPE, SIG_IGN);//当进程向已关闭的socket写入数据时，不会收到SIGPIPE信号，避免进程被意外终止
    utils.addsig(SIGALRM, utils.sig_handler, false);//绑定函数至计时器超时信号
    utils.addsig(SIGTERM, utils.sig_handler, false);//绑定函数至终止信号

    alarm(TIMESLOT);//定时，触发SIGALRM信号

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

//
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    //初始化传入的coonfd
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];//深拷贝用户信息

    //绑定超时处理函数
    timer->cb_func = cb_func;
    time_t cur = time(NULL);          // 获取当前时间戳（Unix秒）
    timer->expire = cur + 3 * TIMESLOT; // 设置超时时刻 = 现在 + 15秒
    users_timer[connfd].timer = timer;  // 把定时器绑定到这个连接上
    utils.m_timer_lst.add_timer(timer); // 把定时器插入有序链表
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    //执行超时函数
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

//处理连接请求
bool WebServer::dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode)//LT模式
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        //添加计时器
        timer(connfd, client_address);
    }

    else//ET模式
    {
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                //开启计时器
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    //reactor 主线程负责事件分发，读写交给线程池处理
    if (1 == m_actormodel)
    {
        if (timer)
        {
            //增加计时器时间防止中途因为超时意外退出
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)//任务已完成
            {
                if (1 == users[sockfd].timer_flag)//发生错误
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    //proactor 主线程读取数据后将请求交给线程池处理
    else
    {
        
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;//超时提醒
    bool stop_server = false;//是否关闭服务端

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;//获取接收到就绪事件的sock的fd

            //处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                bool flag = dealclientdata();
                if (false == flag)
                    continue;
            }
            //发生了关闭、挂起或错误
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            //m_pipefd[0]收到信号同时发生EPOLLIN事件时，触发读事件处理
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                //处理读信号
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                //处理写信号
                dealwithwrite(sockfd);
            }
        }
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}
