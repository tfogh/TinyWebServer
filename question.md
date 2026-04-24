# TinyWebServer 面试题清单

以下题目由浅入深、按主题分类，覆盖架构、网络 I/O、并发模型、HTTP 状态机、性能优化、数据库连接池、定时器与信号、日志与可观测性、调试与修复，以及扩展设计题。每题后包含“被问及概率、难度”和简短的回答要点，便于面试准备。

---

## 1. 项目与架构（基础）

1. 用一两句话介绍这个项目的整体架构和主要模块（可举出关键文件）。 — 概率: 90% — 难度: Easy

回答要点：
- 核心：epoll 非阻塞 I/O + 线程池 + HTTP 状态机（webserver.cpp、http/http_conn.cpp、threadpool/threadpool.h）。
- 辅助：定时器（timer/lst_timer.*）、数据库连接池（CGImysql/sql_connection_pool.*）、日志模块（log/*）。

答:主线程使用epoll阻塞IO分配任务，工作线程采用非阻塞IO执行任务，线程池用于调度工作线程和数据库相关任务。http状态机用于解析和输出http报头。
还配有定时器模块防止等待超时，日志模块用于记录服务器状态信息。


2. 主线程职责是什么？工作线程职责是什么？在哪些函数/文件体现？ — 概率: 90% — 难度: Easy
答:主要负责接受和分配任务给工作线程，工作线程负责具体的任务，包括http解析，数据库交互等

回答要点：
- 主线程：事件分发（epoll_wait）、accept、分发读/写任务（webserver.cpp:eventLoop/dealwithread/dealwithwrite）。
- 工作线程：处理业务（解析 HTTP、数据库交互、构建响应）（threadpool/threadpool.h -> http_conn::process）。

3. 程序启动流程从 main 到事件循环的关键调用链是怎样的？请指出对应的函数或文件。 — 概率: 85% — 难度: Easy

回答要点：
- main.cpp -> Config::parse_arg -> WebServer::init -> log_write/sql_pool/thread_pool/trig_mode/eventListen -> WebServer::eventLoop。
- 关键文件：main.cpp、webserver.cpp、config.cpp、http/http_conn.cpp。

4. 项目如何通过配置（命令行）调整运行行为？常见可调参数有哪些？ — 概率: 75% — 难度: Medium

回答要点：
- 通过 Config::parse_arg 解析 -p -l -m -o -s -t -c -a 等参数。
- 常见：端口(-p)、日志模式(-l)、触发模式(-m)、数据库连接数(-s)、线程数(-t)、并发模型(-a)。

5. 项目采用哪种并发和 I/O 多路复用组合？为什么选择这种组合？ — 概率: 80% — 难度: Medium
采用epoll加非阻塞IO，非阻塞io可以在没有工作的时候保持休息，只有在有具体任务时才被唤醒处理对应任务，这种设计可以有效的应对高并发情况
回答要点：
- epoll + 非阻塞 socket + 线程池（半同步/半反应堆）组合。
- 优点：主线程低 CPU 占用、支持大量并发连接、业务并发交给线程池处理。

---

## 2. epoll 与 I/O（细节）

6. 主线程具体在哪个调用上阻塞等待事件？请用文件名和函数名说明。 — 概率: 90% — 难度: Easy

回答要点：
- 在 webserver.cpp 的 WebServer::eventLoop() 中阻塞于 epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1)。

7. epoll_wait 参数含义是什么？代码中传入的参数如何影响行为？ — 概率: 85% — 难度: Medium

回答要点：
- 参数：epfd、events(输出缓冲)、maxevents、timeout(-1 阻塞、0 不阻塞、>0 最长等待)。
- timeout 控制主线程是否长期阻塞或轮询。

8. LT 与 ET 有何区别？项目中如何通过参数切换 LT/ET？ — 概率: 95% — 难度: Medium

回答要点：
- LT（水平触发）：内核持续报告直到数据读完；ET（边沿触发）：只在状态变化时通知一次，需读尽数据。
- 项目通过 -m 参数映射 0..3 到 LISTENTrigmode/CONNTrigmode（webserver::trig_mode）。

9. EPOLLONESHOT 在项目中为什么会被使用？它解决了什么问题？ — 概率: 90% — 难度: Medium

回答要点：
- 避免同一连接被多个线程同时处理（竞态）。
- 使用 EPOLLONESHOT 后需要在处理完成后调用 modfd 恢复事件。

10. addfd、modfd、removefd 的实现细节是什么？它们设置了哪些 epoll flags？ — 概率: 80% — 难度: Medium

回答要点：
- addfd 设置 EPOLLIN, EPOLLET（可选）、EPOLLRDHUP，并可能加入 EPOLLONESHOT。
- modfd 用于修改事件掩码（例如切换到 EPOLLOUT 或恢复 EPOLLIN）。
- removefd 调用 EPOLL_CTL_DEL 并关闭 fd。

11. 为什么把 listenfd、connfd、以及管道 fd 都设置为非阻塞？ — 概率: 95% — 难度: Easy

回答要点：
- 防止单个慢连接阻塞主线程或工作线程，保证事件驱动模型正确工作（ET 需非阻塞）。

12. 如何处理 EPOLLRDHUP / EPOLLHUP / EPOLLERR 事件？ — 概率: 85% — 难度: Medium

回答要点：
- 在 eventLoop 检查这些事件后调用 deal_timer/close_conn，清理资源并移除定时器。

---

## 3. 并发模型与线程池（实现与问题）

13. 代码里如何实现 Reactor 与 Proactor 两种并发模型？actor_model 参数如何影响流程？ — 概率: 90% — 难度: Medium

回答要点：
- actor_model==1 (Reactor)：主线程分发读写事件到线程池，工作线程执行 read/process/write。
- actor_model==0 (Proactor)：主线程先完成 read 或 write，再把处理交给线程池（append_p）。

14. 线程池的请求队列是如何实现的？使用了哪些同步原语？ — 概率: 85% — 难度: Medium

回答要点：
- 使用 std::list<T*> 作为队列，locker(互斥锁) 保护，sem 信号量唤醒工作线程，cond 在日志队列使用。

15. append 与 append_p 的区别是什么？什么时候选择哪一个？ — 概率: 80% — 难度: Medium

回答要点：
- append(state)：设置 request->m_state 并入队（用于 Reactor，标记读/写）。
- append_p：直接入队（用于 Proactor，数据已在缓冲区）。

16. 线程池中，工作线程的执行流程（worker -> run）是怎样的？ — 概率: 80% — 难度: Medium

回答要点：
- worker 调用 static 入口 -> run 循环等待 sem -> 取队列任务 -> 根据 actor_model 调用 request->process() 或读/写处理。

17. 讨论可能的竞态条件或死锁点，举例说明如何复现与修复。 — 概率: 70% — 难度: Hard

回答要点：
- 竞态：EPOLLONESHOT 未及时 modfd 恢复导致事件丢失；users map 并发访问需锁保护（m_lock）。
- 修复：精确保护共享数据、避免 busy-wait、确保在所有路径上释放锁。

18. 目前的线程池如何优雅退出？如果没有，如何实现优雅关闭？ — 概率: 65% — 难度: Hard

回答要点：
- 当前无显式优雅停止：可添加停止标志、broadcast/sem post 唤醒线程、join 或使用可中断的等待并让线程退出循环。

19. m_queuestat 使用信号量，其作用是什么？为何需要信号量而不是条件变量？ — 概率: 75% — 难度: Medium

回答要点：
- 信号量用于表示队列中可用任务计数，线程 wait 时直接阻塞等待资源；实现简单且适合生产者/消费者场景。

---

## 4. HTTP 状态机与请求处理

20. HTTP 状态机有哪些状态？parse_line、parse_request_line、parse_headers、parse_content 各自负责什么？ — 概率: 95% — 难度: Medium

回答要点：
- 状态：CHECK_STATE_REQUESTLINE / HEADER / CONTENT。
- parse_line 判断行结束；parse_request_line 解析方法/URL/版本；parse_headers 解析 Connection/Content-Length/Host；parse_content 读取 body 后进入处理。

21. 如何解析 POST 请求的 body？代码里是如何判断 body 读取完毕的？ — 概率: 85% — 难度: Medium

回答要点：
- 通过 Content-Length 记录 m_content_length，read 直到 m_read_idx >= m_checked_idx + m_content_length，parse_content 返回 GET_REQUEST。

22. CGI 登录/注册逻辑在哪实现？数据如何从 HTTP 请求映射到 SQL 操作？ — 概率: 80% — 难度: Medium

回答要点：
- 在 http_conn::do_request 中处理 cgi 标志，解析 m_string 中 user/passwd，使用 mysql_query 插入或比对 users map。

23. process() 的主要流程是什么？在不同的 HTTP_CODE 下如何响应？ — 概率: 90% — 难度: Medium

回答要点：
- process 调用 process_read -> 得到 HTTP_CODE -> process_write 构造响应 -> set EPOLLOUT，或在 NO_REQUEST 时继续等待。

24. 在 parse_request_line 中，如何兼容带有完整 URL（http://host/path）的情况？ — 概率: 75% — 难度: Medium

回答要点：
- 判断 "http://" 或 "https://" 前缀，跳过 host 部分找到第一个 '/'，再解析路径。

25. 代码中如何处理 Connection: keep-alive？在哪里决定是否长连接？ — 概率: 80% — 难度: Medium

回答要点：
- parse_headers 解析 Connection 字段并设置 m_linger；process_write 根据 m_linger 决定是否在发送完毕后保持连接或关闭。

---

## 5. 文件 I/O 与性能优化

26. 为什么使用 mmap 来读取静态文件？mmap 有哪些优缺点？ — 概率: 85% — 难度: Medium

回答要点：
- mmap 减少拷贝（零拷贝潜力），配合 writev 能高效发送大文件。缺点：对 address space 要求高、文件过大可能耗尽虚拟地址空间，需要注意并发 mmap/unmap 成本。

27. writev 的作用是什么？在项目中怎样配合 writev 使用？ — 概率: 80% — 难度: Medium

回答要点：
- writev 聚合多个 buffer（响应头、body）到一次系统调用，减少 syscalls，提高吞吐。

28. 请求大文件（图片/视频）时需要注意哪些资源/系统限制？ — 概率: 70% — 难度: Medium

回答要点：
- 文件描述符上限、虚拟地址空间、内存锁竞用、send buffer、IO 带宽和磁盘并发读取。

29. 如果 m_file_stat.st_size 很大，会对内存或 address space 有何影响？如何改进？ — 概率: 65% — 难度: Hard

回答要点：
- 大文件 mmap 可能占用大量虚拟地址，影响其它 mmap 操作；改进：分段传输（sendfile/分块 mmap/读写），或使用 sendfile 零拷贝传输。

30. unmap() 在何处被调用？如果忘记 unmap 会怎样？ — 概率: 70% — 难度: Medium

回答要点：
- 在发送完成或连接关闭时调用 unmap()（http_conn::unmap）；忘记会导致虚拟内存泄漏和 address space 漏洞。

---

## 6. MySQL 与连接池

31. connection_pool 的实现思路是什么？如何保证线程安全？ — 概率: 75% — 难度: Medium

回答要点：
- 预创建连接放入 list，使用 locker 保护操作，使用 sem(reserve) 管理可用连接计数。

32. connectionRAII 的作用是什么？举例说明其在代码中的用法。 — 概率: 70% — 难度: Medium

回答要点：
- RAII 在构造时从池中取连接，析构时自动释放回池，保证异常安全，见 connectionRAII 构造/析构。

33. 当 MySQL 查询失败时，代码如何处理？是否有重试或错误回退？ — 概率: 60% — 难度: Medium

回答要点：
- 当前多为记录日志（LOG_ERROR）并返回错误页面，未实现自动重试或连接重建逻辑，需要增强健壮性。

34. 如何调整数据库连接池的大小以应对并发？会带来哪些权衡？ — 概率: 65% — 难度: Medium

回答要点：
- 增大连接数提升并发数据库访问，但会占用 DB 资源；权衡网络延迟、数据库吞吐和内存消耗。

35. 连接池如何处理连接断开或超时的情况？当前实现是否健壮？ — 概率: 60% — 难度: Hard

回答要点：
- 当前实现没有主动检测连接失效，依赖 mysql_real_connect 成功与否；应加入连接健康检查与重连策略。

---

## 7. 定时器、信号与超时回收

36. 定时器（sort_timer_lst）如何组织与触发超时回调？复杂度如何？ — 概率: 70% — 难度: Medium

回答要点：
- 使用升序双向链表，新增/调整/删除为 O(n) 在最坏情况下；tick 从 head 开始触发已到期计时器。

37. Utils::sig_handler 为什么通过 socketpair 发送信号？有什么好处？ — 概率: 80% — 难度: Medium

回答要点：
- 在信号处理函数只做最小化工作（可重入），通过 socketpair 将信号信息写入管道，主线程通过 epoll 统一处理，避免信号处理限制。

38. cb_func 的作用是什么？超时处理流程是怎样的？ — 概率: 75% — 难度: Medium

回答要点：
- cb_func 关闭超时连接（epoll_ctl DEL + close），由 sort_timer_lst.tick 调用，实际回收 fd 和减少连接计数。

39. 如何调整单个连接的超时时间？在哪些地方被延长（adjust_timer）？ — 概率: 70% — 难度: Medium

回答要点：
- 在 WebServer::timer 创建时设置 expire，dealwithread/dealwithwrite 成功交互时调用 adjust_timer 延长 expire。

40. 如果定时器大量增长（很多连接），tick() 的性能是否会成为瓶颈？如何优化？ — 概率: 65% — 难度: Hard

回答要点：
- 链表遍历 O(n) 可能成为瓶颈；优化：使用时间轮、最小堆或分层计时器以降低平均复杂度。

---

## 8. 日志与可观测性

41. 日志模块如何支持同步与异步模式？异步模式内部用到了哪些数据结构与线程？ — 概率: 60% — 难度: Medium

回答要点：
- 异步模式开启后使用 block_queue<string> 阻塞队列，并创建单独线程 flush_log_thread 从队列写入日志。

42. 日志如何按天/按行切分文件？为什么这样做？ — 概率: 55% — 难度: Easy

回答要点：
- 按日期和行数（m_today、m_split_lines）切分，便于日志管理、归档与避免单文件过大。

43. 如果日志写入失败或文件打开失败，程序如何处理？ — 概率: 50% — 难度: Medium

回答要点：
- Log::init 返回 false，则 WebServer::log_write 会将 m_close_log 置为 1，后续日志写入被禁用以避免空指针。

44. 如何在不影响性能的前提下增加更多运行时统计（QPS、连接数、延迟分位）？ — 概率: 55% — 难度: Medium

回答要点：
- 使用原子计数器记录计数，周期性采样并异步上报；避免在热路径阻塞同步写盘。

---

## 9. 错误处理、资源管理与安全

45. close_conn、unmap、DestroyPool 等资源释放函数在哪些边界条件下被调用？ — 概率: 70% — 难度: Medium

回答要点：
- close_conn 在连接结束或异常时调用，unmap 在发送完或关闭时调用，DestroyPool 在连接池析构时调用以关闭 DB 连接。

46. 代码中存在哪些潜在的内存/FD 泄漏点？如何检测与修复？ — 概率: 70% — 难度: Hard

回答要点：
- 漏洞例子：未 unmap 的 mmap、未关闭的 fd、malloc 未 free。检测用 valgrind、lsof、ASAN、系统监控。修复：RAII、析构中释放、统一错误路径清理。

47. main.cpp 中硬编码数据库凭证有哪些风险？如何改进为更安全的配置方式？ — 概率: 85% — 难度: Medium

回答要点：
- 风险：凭证泄露、不可移植。改进：环境变量、外部配置文件（权限保护）、或使用秘密管理服务。

48. 解析请求与字符串处理处（如 strcpy、strcat）的安全性如何？可能的缓冲区溢出如何避免？ — 概率: 85% — 难度: Hard

回答要点：
- 当前使用 strcpy/strcat 存在越界风险。应使用 strncpy/strncat 或 std::string，并检查边界/长度。

49. 是否需要对用户输入（用户名/密码）进行转义或最大长度校验？为什么？ — 概率: 80% — 难度: Medium

回答要点：
- 必须校验长度并做输入净化，防止 SQL 注入/缓冲区溢出；应使用预处理语句或转义。

---

## 10. 压测、性能分析与调优

50. README 中给出了不同模式下的 QPS（如 ~90k QPS），你如何复现这些压测？需要注意哪些环境与配置？ — 概率: 75% — 难度: Medium

回答要点：
- 配置相同内核参数（ulimit, somaxconn, open files）、关闭日志、用 webbench 或 wrk，多核/独立机器、网络延迟控制。

51. 在压测中如何定位 CPU、内存或 I/O 瓶颈？会使用哪些工具？ — 概率: 75% — 难度: Hard

回答要点：
- 工具：top/htop、perf、vmstat、iostat、strace、tcpdump、pprof 或 flamegraph。定位热点后优化代码或 IO。

52. 如果在高并发下出现大量 EAGAIN 或短时间内大量连接超时，你会如何排查？ — 概率: 70% — 难度: Hard

回答要点：
- 检查内核 fd 限制、socket 缓冲区、epoll 触发模式、线程池是否饱和、是否忘记 modfd 恢复事件。

53. 如果想把并发提升到 100k 连接，需要做哪些操作（内核 / 应用 / 架构）？ — 概率: 65% — 难度: Hard

回答要点：
- 提升 fd 限制、优化内核参数（net.core.somaxconn、tcp_tw_reuse）、采用更高效事件分发、拆分服务、使用负载均衡。

---

## 11. 调试 / 修复题（上机或代码改动）

54. 代码里 Reactor 模式下使用了 busy-wait（轮询 improv 字段）。请定位该逻辑并修改为等待/通知机制或其他更优方案。 — 概率: 60% — 难度: Hard

回答要点：
- 定位：webserver.cpp 的 dealwithread/dealwithwrite 中的 while(1) 检查 users[sockfd].improv。
- 方案：使用条件变量/事件或在工作线程设置回调并通过管道/epoll 通知主线程完成。

55. 修复一个边界 bug：当读取的请求体长度超过 READ_BUFFER_SIZE 时，当前实现会怎样？请修复并说明测试方法。 — 概率: 55% — 难度: Hard

回答要点：
- 问题：会溢出或丢失数据。修复：实现动态缓冲（可 grow 的 vector/string）或循环读取直到 Content-Length 完成并检查上限。
- 测试：构造大于 READ_BUFFER_SIZE 的 POST，验证服务器正确处理或返回 413。

56. 将数据库凭证从 main.cpp 的硬编码改为环境变量或配置文件读取，并保证对旧行为兼容。 — 概率: 70% — 难度: Medium

回答要点：
- 使用 getenv 或读取 config 文件（优先环境变量），回退到硬编码值（仅测试环境）并记录警告日志。

57. 为线程池实现优雅关停：实现一个接口使主线程可以等待工作线程完成当前任务并退出。 — 概率: 50% — 难度: Hard

回答要点：
- 添加停止标志、向每个线程 post 信号以唤醒、线程检查标志后退出、主线程 join/等待线程完成。

58. 增加一个单元测试用例验证 HTTP 状态机能正确解析一个简单的 POST 请求（提供测试输入和期望输出）。 — 概率: 50% — 难度: Medium

回答要点：
- 构造包含请求行/头/body 的原始字符串，调用 process_read 并断言返回 GET_REQUEST，以及字段解析正确。

---

## 12. 深度设计与扩展（开放性问题）

59. 如果要在该服务器上增加 HTTPS 支持（非阻塞），整体设计如何改动？哪些地方最复杂？ — 概率: 45% — 难度: Hard

回答要点：
- 需要集成非阻塞 TLS（如 OpenSSL 非阻塞 API 或 BoringSSL），处理握手状态机、证书管理与加密缓冲，最复杂在握手/重入与性能影响上。

60. 讨论如何把单机 Web 服务器扩展为分布式（负载均衡、共享会话、静态资源 CDN）。 — 概率: 50% — 难度: Medium

回答要点：
- 使用负载均衡（L4/L7）、共享会话（Redis）、静态资源上 CDN/对象存储、服务拆分与健康检查。

61. 如果要改造为完全使用 ET（边缘触发）并且移除 EPOLLONESHOT，需做哪些修改？潜在风险是什么？ — 概率: 50% — 难度: Hard

回答要点：
- 需要保证在事件到达时循环读取/写尽所有数据并正确处理 EAGAIN；风险是忘记读尽数据导致事件丢失或竞争问题。

62. 设计一个监控与告警方案（CPU、FD、QPS、慢请求、错误率）。哪些指标最关键？ — 概率: 60% — 难度: Medium

回答要点：
- 指标：active connections、open fds、QPS、latency P50/P95/P99、error rate、thread pool queue length；上报到 Prometheus/Grafana，设置阈值告警。

---

## 13. 行为与经验类问题

63. 你在实现这些模块时最困难的地方是什么？当时是如何解决的？ — 概率: 70% — 难度: Medium

回答要点：
- 可谈 epoll ET 细节、并发竞态、资源泄露调试经验和使用工具（valgrind/strace）解决问题。

64. 如果让你重构或优化本项目，你会优先改哪个模块，为什么？ — 概率: 70% — 难度: Medium

回答要点：
- 优先改线程池与事件通知（去掉 busy-wait）、增强连接池健壮性和输入安全；这些改动收益高且风险可控。

65. 在团队协作中，你如何保证对这样低层网络代码的变更不会引入回归？ — 概率: 65% — 难度: Medium

回答要点：
- 建立回归测试/压测脚本、代码审查规范、静态检查、持续集成和高覆盖率的集成测试。

---

## 附：面试评分要点（考官备忘）

- 是否能准确描述 epoll 工作原理与 LT/ET 差异，并能在代码中定位实现位置。
- 是否理解 Reactor / Proactor 的设计权衡，并能解释代码中两者的实现差别。
- 能否指出并改进线程同步、busy-wait 与资源回收的问题（能写出修改方案）。
- 对 mmap/writev、数据库连接池、定时器与信号机制的理解深度。
- 对安全（输入校验、凭证管理）和性能（高并发、压测）体现出工程意识。

---

*文件位置：/home/sen/TinyWebServer/question.md*
