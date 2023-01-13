//
// Created by 梁隽宁 on 2022/11/24.
//

#ifndef MAIN_C_LOCKER_H
#define MAIN_C_LOCKER_H
#include <exception>
#include <pthread.h>
#include <semaphore.h>
class sem
{
public:
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            throw std::exception();
        }
    }
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    bool wait()
    {
        return sem_wait(&m_sem) == 0;//等待信号量，信号量值减1，如果信号量值为0，sem_wait将被阻塞，直到信号量非0
    }
    bool post()
    {
        return sem_post(&m_sem) == 0;//增加信号量，信号量值加1
    }

private:
    sem_t m_sem;
};
class locker//互斥锁类
{
public:
    locker()
    {
        if (pthread_mutex_init(&m_mutex, nullptr) != 0)
        {
            //初始化互斥锁，第二个参数指定互斥锁属性，设为nullptr代表使用默认属性
            throw std::exception();//抛出异常，记得要exception头文件
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);//销毁互斥锁
    }
    bool lock()
    {
        //以原子操作方式给互斥锁上锁，给已上锁的锁上锁会阻塞
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock()
    {
        //以原子操作方式给互斥锁解锁
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t *get()//指针函数，返回值为指针
    {
        //返回互斥锁地址
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;//互斥锁变量，先声明
};
class cond//条件变量类
{//条件变量提供一种通知机制，当某个变量达到某个值时，唤醒等待这个变量的线程
public:
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            //pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex)
    {
        /*pthread_cond_wait()函数一进入wait状态就会自动release mutex。
         * 当pthread_cond_signal()或pthread_cond_broadcast，
         * 使pthread_cond_wait()解除阻塞时，
         * 该线程又自动获得该mutex*/
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)//定时解除阻塞
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()//单个唤醒
    {
        return pthread_cond_signal(&m_cond) == 0;
    }
    bool broadcast()//以广播的形式全部唤醒
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;//条件变量，符合条件就通知
};
#endif //MAIN_C_LOCKER_H
