//
// Created by 梁隽宁 on 2022/11/24.
//
#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
    this->CurConn = 0;
    this->FreeConn = 0;
}

connection_pool *connection_pool::GetInstance()
{
    static connection_pool connPool;//对象内自行创建唯一实例
    return &connPool;//把唯一实例以引用的方式传出去
}

//构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, unsigned int MaxConn)
{
    //初始化数据库信息
    this->url = url;
    this->Port = Port;
    this->User = User;
    this->PassWord = PassWord;
    this->DatabaseName = DBName;

    lock.lock();
    //创建MaxConn条数据库连接
    for (int i = 0; i < MaxConn; i++)
    {
        MYSQL *con = NULL;//创建mysql指针类型变量
        con = mysql_init(con);//函数用来分配或者初始化一个MYSQL对象，用于连接mysql服务端

        if (con == NULL)
        {
            cout << "Error:" << mysql_error(con);
            exit(1);
        }
        //调用mysql_real_connect函数连接Mysql数据库
        //MYSQL * STDCALL mysql_real_connect（MYSQL *mysql， const char *host，const char *user，const char *passwd，const char *db，unsigned int port，const char *unix_socket，unsigned long clientflag）
        con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

        if (con == NULL)
        {
            cout << "Error: " << mysql_error(con);//mysql_error()返回最近被调用的MySQL函数的出错消息
            exit(1);
        }
        //更新连接池和空闲连接数量
        connList.push_back(con);//放入连接对象
        ++FreeConn;
    }
    //将信号量初始化为最大连接次数
    reserve = sem(FreeConn);

    this->MaxConn = FreeConn;

    lock.unlock();
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
    MYSQL *con = NULL;//创建一个局部mysql变量

    if (connList.size() == 0)
        return NULL;
    //取出连接，信号量原子减1，为0则等待
    reserve.wait();

    lock.lock();

    con = connList.front();
    connList.pop_front();
    //这里的两个变量，并没有用到，非常鸡肋...
    --FreeConn;
    ++CurConn;

    lock.unlock();
    return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
    if (NULL == con)
        return false;

    lock.lock();

    connList.push_back(con);
    ++FreeConn;
    --CurConn;

    lock.unlock();
    //释放连接原子加1
    reserve.post();
    return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{

    lock.lock();
    if (connList.size() > 0)
    {
        //通过迭代器遍历，关闭数据库连接
        list<MYSQL *>::iterator it;
        for (it = connList.begin(); it != connList.end(); ++it)
        {
            MYSQL *con = *it;
            mysql_close(con);//mysql_close()关闭一个服务器连接
        }
        CurConn = 0;
        FreeConn = 0;
        //清空list
        connList.clear();

        lock.unlock();
    }

    lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
    return this->FreeConn;
}
//RAII机制销毁连接池
connection_pool::~connection_pool()
{
    DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool)
{
    *SQL = connPool->GetConnection();
    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
    poolRAII->ReleaseConnection(conRAII);//连接池对象调用释放函数，把要释放的数据库连接传给ReleaseConnection()；
}