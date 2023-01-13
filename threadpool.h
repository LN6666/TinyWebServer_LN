//
// Created by 梁隽宁 on 2022/11/24.
//

#ifndef MAIN_C_THREADPOOL_H
#define MAIN_C_THREADPOOL_H
#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"
#include "sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request);//添加任务的函数

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();//线程池启动函数

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    bool m_stop;                //是否结束线程
    connection_pool *m_connPool;//数据库
};
template <typename T>
threadpool<T>::threadpool( connection_pool *connPool, int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];//线程池数组动态的创建
    if (!m_threads)
        throw std::exception();
    //创建thread_number个线程并将它们设置为线程脱离
    for (int i = 0; i < thread_number; ++i)
    {
        printf("create the %dth thread\n",i);
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            //创建失败，要销毁数组释放空间
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
}
template <typename T>
bool threadpool<T>::append(T *request)
{
    m_queuelocker.lock();//添加任务要上锁
    if (m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock();//如果请求队列的大小大于最大能处理数
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();//增加一个任务也要增加一个信号量
    return true;
}
template <typename T>
void *threadpool<T>::worker(void *arg)//线程执行的回调函数
{
    threadpool *pool = (threadpool *)arg;//这里的arg为传入的形参，为47行的this指针，并进行强制类型转换
    pool->run();
    return pool;//函数返回值为void*即任意类型，这里返回pool实际上没有意义
}
template <typename T>
void threadpool<T>::run()//启动线程池函数
{
    while (!m_stop)//直到m_stop为true才停止循环(m_stop为true，！m_stop为false)
    {
        m_queuestat.wait();//信号量随着循环-1到0，就阻塞，直到又有任务要添加
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();//取队列的第一个任务出来
        m_workqueue.pop_front();//删除队列的第一个任务
        m_queuelocker.unlock();//解锁
        if (!request)//如果任务为空，那就重新循环遍历
            continue;

        connectionRAII mysqlcon(&request->mysql, m_connPool);
        /*从连接池中取出一个数据库连接
        request->mysql = m_connPool->GetConnection();

        //process(模板类中的方法,这里是http类)进行处理
        request->process();

        //将数据库连接放回连接池
        m_connPool->ReleaseConnection(request->mysql);*/
        request->process();
    }
}
#endif //MAIN_C_THREADPOOL_H
