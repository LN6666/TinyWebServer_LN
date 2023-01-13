//
// Created by 梁隽宁 on 2022/11/24.
//

#ifndef MAIN_C_SQL_CONNECTION_POOL_H
#define MAIN_C_SQL_CONNECTION_POOL_H
#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "locker.h"

using namespace std;

class connection_pool//数据库连接池类
{
public:
    MYSQL *GetConnection();				 //获取数据库连接
    bool ReleaseConnection(MYSQL *conn); //释放连接
    int GetFreeConn();					 //获取连接
    void DestroyPool();					 //销毁所有连接

    //单例模式
    static connection_pool *GetInstance();//局部静态变量单例模式(使用局部静态变量懒汉模式创建连接池)
    /*单例模式有 3 个特点：
     * 单例类只有一个实例对象;
     * 该单例对象必须由单例类自行创建；
     * 单例类对外提供一个访问该单例的全局访问点；*/

    //构造初始化
    void init(string url, string User, string PassWord, string DataBaseName, int Port, unsigned int MaxConn);

    connection_pool();
    ~connection_pool();

private:
    unsigned int MaxConn;  //最大连接数
    unsigned int CurConn;  //当前已使用的连接数
    unsigned int FreeConn; //当前空闲的连接数

private:
    locker lock;
    list<MYSQL *> connList; //连接池
    sem reserve;

private:
    string url;			 //主机地址
    string Port;		 //数据库端口号
    string User;		 //登陆数据库用户名
    string PassWord;	 //登陆数据库密码
    string DatabaseName; //使用数据库名
};

class connectionRAII//不直接调用获取和释放连接的接口，将其封装起来，通过RAII机制进行获取和释放
{
public:
    //双指针对MYSQL *con修改
    connectionRAII(MYSQL **con, connection_pool *connPool);//数据库连接本身是指针类型，所以参数需要通过双指针才能对其进行修改
    ~connectionRAII();

private:
    MYSQL *conRAII;//数据库连接
    connection_pool *poolRAII;//数据库连接池
};
#endif //MAIN_C_SQL_CONNECTION_POOL_H
