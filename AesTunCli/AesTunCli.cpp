// AesTunCli.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <vector>
#include <atomic>
#include "../aes.h"
#include "../global.h"

#pragma comment(lib,"ws2_32.lib")

CSocketPairMap mapSocketPair;   // 提供服务和访问者对

// 本地服务监听端口信息
std::string strHost = "127.0.0.1";
int nPort = 12345; //808;
// 后端服务信息
std::string strSvrHost = "127.0.0.1";
int nSvrPort = 12080;

// AES key
std::string strDefaultKey = "asdf1234567890";
std::string strAesKey = strDefaultKey;

std::atomic<int> nVIDCreator(0);

void CloseSocketPair(int nSvrNum)
{
    CSocketPairMap::iterator iter = mapSocketPair.find(nSvrNum);
    if (iter != mapSocketPair.end())
    {
        CSocketPair pair = iter->second;
        mapSocketPair.erase(iter);
        CloseSocketPair(pair);
    }
}

void ProcessRead(CSocketPairMap& mapSocketPair, fd_set& fdRead)
{
    int nRet = 0;
    std::vector<int> vecDelSvrNum;
    // 处理所有socket对的收数据
    for (iterSockets iter = mapSocketPair.begin(); iter != mapSocketPair.end(); iter++)
    {
        CSocketPair& pair = iter->second;
        if (pair.pVistor && pair.pServer && FD_ISSET(pair.pVistor->sock, &fdRead))
        {
            CLfrpSocket* pSocket = pair.pVistor;
            CLfrpSocket* pTunSocket = pair.pServer;
            int nSeq = GetNextSeq(SEQ_CLIENT, pSocket->sock);
            //开始recv
            char Buffer[RECV_BUFFER_SIZE];
            int nRet = recv(pSocket->sock, Buffer, RECV_BUFFER_SIZE, 0);
            if (nRet == SOCKET_ERROR || nRet == 0)   // 远端断开触发=0
            {
                //CloseSocketPair(pSocket->nServiceNumber);
                vecDelSvrNum.push_back(pSocket->nServiceNumber);
                continue;
            }
            else if (nRet > 0)
            {
                AesTunMakeSendPack(pTunSocket, Buffer, nRet, nSeq);
                pTunSocket->Op |= OP_WRITE;
            }
        }

        if (pair.pVistor && pair.pServer && FD_ISSET(pair.pServer->sock, &fdRead))
        {
            CLfrpSocket* pSocket = pair.pVistor;
            CLfrpSocket* pTunSocket = pair.pServer;
            //开始recv
            char Buffer[RECV_BUFFER_SIZE];
            int nRet = recv(pTunSocket->sock, Buffer, RECV_BUFFER_SIZE, 0);
            if (nRet == SOCKET_ERROR || nRet == 0)   // 远端断开触发=0
            {
                vecDelSvrNum.push_back(pTunSocket->nServiceNumber);
                continue;
            }
            else if (nRet > 0)
            {
                AesTunRecvAndMoveDate(pTunSocket, pSocket, Buffer, nRet, pair);
                pSocket->Op |= OP_WRITE;
            }
        }
    }

    for (size_t i = 0; i < vecDelSvrNum.size(); i++)
    {
        CloseSocketPair(vecDelSvrNum[i]);
    }
}

void ProcessWrite(CSocketPairMap& mapSocketPair, fd_set& fdWrite)
{
    int nRet = 0;
    for (iterSockets iter = mapSocketPair.begin(); iter != mapSocketPair.end(); iter++)
    {
        CSocketPair& pair = iter->second;
        if (pair.pServer && FD_ISSET(pair.pServer->sock, &fdWrite))
        {
            if (pair.pServer->Op &= OP_WRITE)
            {
                AesTunSendDate(pair.pServer, true, true, pair);
            }
        }
        if (pair.pVistor && FD_ISSET(pair.pVistor->sock, &fdWrite))
        {
            if (pair.pVistor->Op &= OP_WRITE)
            {
                AesTunSendDate(pair.pVistor, false, false, pair);
            }
        }
    }
}

int mainSelect(SOCKET& sockListen)
{
    int nRet = 0;
    CLfrpSocket sockTun;
    CSocketMap mapSvr;    // 业务服务代理连接

    unsigned int uLastTunSec = GetCurSecond();
    unsigned int uLastHeartBeatSec = GetCurSecond();
    CLfrpSocket sListen;
    sListen.sock = sockListen;
    while (true)
    {
        //循环判断是否有请求需要处理
        fd_set fdRead, fdWrite;
        while (true)
        {
            bool bSetFD = false;
            FD_ZERO(&fdRead);
            FD_ZERO(&fdWrite);
            FD_SET(sListen.sock, &fdRead);
            SOCKET maxSock = sListen.sock;
            for (iterSockets iter = mapSocketPair.begin(); iter != mapSocketPair.end(); iter++)
            {
                CSocketPair pair = iter->second;
                // 每对socket都要置状态
                if (pair.pServer && pair.pServer->sock != INVALID_SOCKET)
                {
                    LfrpSetFD(pair.pServer, fdRead, fdWrite);
                    maxSock = max(maxSock, pair.pServer->sock);
                }
                if (pair.pVistor && pair.pVistor->sock != INVALID_SOCKET)
                {
                    LfrpSetFD(pair.pVistor, fdRead, fdWrite);
                    maxSock = max(maxSock, pair.pVistor->sock);
                }
            }

            timeval timevalSelect = { 5, 0 };
            //这个操作会被阻塞
#ifdef _WIN32
            nRet = select(0, &fdRead, &fdWrite, NULL, &timevalSelect);
#else
            nRet = select(maxSock + 1, &fdRead, &fdWrite, NULL, &timevalSelect);
#endif

            if (FD_ISSET(sockListen, &fdRead))
            {
                //socket可用了，这时accept一定会立刻返回成功或失败 这里需要处理最大连接数
                SOCKET sockNewClient = accept(sockListen, NULL, NULL);
                if (sockNewClient != INVALID_SOCKET)
                {
                    PRINT_INFO("%s Cli %s,%d: accept new socketID %d\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sockNewClient);

                    int enable = 1;
                    if (setsockopt(sockNewClient, IPPROTO_TCP, TCP_NODELAY, (char*)&enable, sizeof(enable)) == SOCKET_ERROR)
                    {
                        PRINT_ERROR("%s Cli %s,%d: accept socket setopt TCP_NODELAY error\n", GetCurTimeStr(), __FUNCTION__, __LINE__);
                    }

                    // 新的Client连接过来，配套一条连到业务服务
                    CLfrpSocket* pCliSocket = new CLfrpSocket;
                    pCliSocket->sock = sockNewClient;
                    pCliSocket->nSocketID = sockNewClient;
                    //pCliSocket->nSocketID = pCliSocket->sock;             // 设置nSocketID
                    pCliSocket->nServiceNumber = nVIDCreator++;
                    if (pCliSocket->nServiceNumber == INVALID_SOCKET)
                    { // 0是默认值，不使用，循环到了重新分配
                        pCliSocket->nServiceNumber = nVIDCreator++;
                    }
                    pCliSocket->nSocketID = pCliSocket->nServiceNumber;

                    CLfrpSocket* pSvrSocket = new CLfrpSocket;
                    pSvrSocket->nServiceNumber = pCliSocket->nServiceNumber;
                    pSvrSocket->nSocketID = pSvrSocket->nServiceNumber;
                    CSocketPair pair;
                    pair.pVistor = pCliSocket;
                    pair.pServer = pSvrSocket;
                    if (ConnectSocket(pSvrSocket->sock, strSvrHost.c_str(), nSvrPort) != 0)
                    {
                        printf("%s connect() Svr Failed: %d\n", GetCurTimeStr(), WSAGetLastError());
                        // 连接业务服务失败，需要通知client侧清掉这条连接
                        CloseSocketPair(pair);
                    }
                    else
                    {
                        mapSocketPair.insert(std::make_pair(pCliSocket->nServiceNumber, pair));
                    }
                }
                //break;
            }

            //其他socket可用了，判断哪些能读，哪些能写
#ifdef _WIN32
            if (fdRead.fd_count > 0)
#endif
            {
                ProcessRead(mapSocketPair, fdRead);
            }
#ifdef _WIN32
            if (fdWrite.fd_count > 0)
#endif
            {
                ProcessWrite(mapSocketPair, fdWrite);
            }
        }
    }

    return 0;
}

int main(int argc, char** argv)
{
    int nRet = 0;
    PRINT_ERROR("%s Cli used as 'AesTunCli -p ListenPort -sh ServerHost -sp ServerPort -k AESKey', default is 'AesTunCli -p %d -sh %s -sp %d -k %s'\n", GetCurTimeStr(), nPort, strSvrHost, nSvrPort, strAesKey.c_str());
    int i = 0;
    for (i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 && i + 1 <= argc)
        {
            i++;
            strHost = argv[i];
        }
        else if (strcmp(argv[i], "-p") == 0 && i + 1 <= argc)
        {
            i++;
            nPort = atoi(argv[i]);
        }
        else if (strcmp(argv[i], "-sh") == 0 && i + 1 <= argc)
        {
            i++;
            strSvrHost = argv[i];
        }
        else if (strcmp(argv[i], "-sp") == 0 && i + 1 <= argc)
        {
            i++;
            nSvrPort = atoi(argv[i]);
        }
        else if (strcmp(argv[i], "-k") == 0 && i + 1 <= argc)
        {
            i++;
            strAesKey = argv[i];
        }
    }

    if (strAesKey == strDefaultKey)
    {
        PRINT_ERROR("Warning: you should change default aes key!!!\n");
    }

    // 初始化AES密钥信息
    CAES::GlobalInit(strAesKey.c_str());

    // 初始化socket
    InitSocket();
    
    InitSection("Cli");
    InitLog("./Cli.txt");

    // 监听端口
    SOCKET sockListen = INVALID_SOCKET;
    nRet = ListenSocket(sockListen, strHost.c_str(), nPort);
    if (nRet != 0)
    {
        return 1;
    };
    mainSelect(sockListen);


    // 初始化epooll工作线程相关
    //SetBusWorkerCallBack(CliRead, CliWrite, CliClose, CliTrans, CliPostAccept, CliTimer);
    //InitWorkerThreads();
    //pSocketPairMapAry = new CSocketPairMap[nThreadCount];

    //// 监听端口
    //SOCKET sockListen = INVALID_SOCKET;
    //nRet = EpollListenSocket(epollfd, sockListen, strHost.c_str(), nPort);
    //if (nRet != 0)
    //{
    //    return 1;
    //};

    //mainEpoll(epollfd, sockListen);
    //bExitPorcess = true;
}

