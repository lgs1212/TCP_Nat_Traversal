#include "../include/natchecker/NatCheckerServer.h"
#include "../include/socket/ClientSocket.h"
#include "../socket/DefaultClientSocket.h"
#include "../socket/DefaultServerSocket.h"
#include "../include/socket/ReuseSocketFactory.h"
#include "../include/transmission/TransmissionData.h"
#include "../include/transmission/TransmissionProxy.h"
#include "../include/database/DataBase.h"
#include "../include/Log.h"

#include <sys/select.h>
#include <errno.h>

#include <thread>
#include <string>

extern int errno;

using namespace std;
using namespace Lib;

typedef nat_type::filter_type filter_type;
typedef nat_type::map_type map_type;

NatCheckerServer::NatCheckerServer(const ip_type& main_addr, port_type main_port, const ip_type& another_addr, port_type another_port)
    :m_main_addr(main_addr),m_main_port(main_port),
      m_another_addr(another_addr),m_another_port(another_port){
    m_main_server = ReuseSocketFactory::GetInstance()->GetServerSocket();
}

NatCheckerServer::~NatCheckerServer(){
    if(m_main_server)
        delete m_main_server;
}

bool NatCheckerServer::setListenNum(size_t num){
    if(!m_main_server)
        return false;

    if(!m_main_server->isBound() && !m_main_server->bind(m_main_addr,m_main_port))
        return false;

    return !m_main_server->isListen() && m_main_server->listen(num);
}

bool NatCheckerServer::setDataBase(DataBase<DataRecord>* database){
    CHECK_PARAMETER_EXCEPTION(database);

    if(m_database)
        return false;

    m_database = database;
    return true;
}

void NatCheckerServer::handle_request(ClientSocket* client){
    TransmissionProxy proxy(client);
    TransmissionData data = proxy.read();

	// 先取出 Client 发送过来的本机地址，对比这边观察到的外部地址，如果外部地址与 Client 发送过来的它那边观察到的地址相同，
	// 表示 Client 处于公网中，可以停止 NAT 类型检测。如果不相同，则先进行 NAT 的 Filter 类型检测(Client那边已经开启了监听，等待连接)，
	// Filter 具体检测过程见下。检测完成后，接着要 Client 配合探测 NAT 的 MAP 类型以及端口映射规律，即不断地让 Client 连接服务器要它连接的端口
	// 服务器检查它连接过来的外部地址，若不明确类型及规律，继续让 Client 连接新端口，直到 STUN 服务器探测出需要的了，就发送停止探测的信息
	// 服务器可将整个过程获得到的信息存储起来以便其他用途
	
    if(!data.isMember(LOCAL_IP) || !data.isMember(LOCAL_PORT) || !data.isMember(IDENTIFIER))
        return;

    ip_type local_ip = data.getString(LOCAL_IP);
    port_type local_port = data.getInt(LOCAL_PORT);

    ip_type ext_ip = client->getPeerAddr();
    port_type ext_port = client->getPeerPort();

    DataRecord record;
    record.setIdentifier(data.getString(IDENTIFIER));

    Address addr(local_ip,local_port);
    record.setLocalAddress(addr);

    addr.ip = ext_ip;
    addr.port = ext_port;
    record.setExtAddress(addr);

    log("NatCheckerServer: ","local_ip: ",local_ip);
    log("NatCheckerServer: ","local_port: ",local_port);

    log("NatCheckerServer: ","ext_ip: ",ext_ip);
    log("NatCheckerServer: ","ext_port: ",ext_port);

    // 这是很奇怪的现象，IP 相同端口却改变了
    if(local_ip == ext_ip && local_port != ext_port){
        log("NatCheckerServer: ","Warning: local IP is equal to extern IP but local port is different from extern port.");
    }

    if(local_ip == ext_ip && local_port == ext_port)	// 内外地址相同，处于公网中，停止检测
    {
        data.clear();
        data.add(CONTINUE,false);
        proxy.write(data);

        record.setNatType(nat_type(false));
    }
    else
    {   // 内外地址不同，存在 NAT，进行 NAT 的 Filter 类型检测 ： 先从 IP2:Port1 对客户端外网地址发起连接
    	// 若连接成功，表示 Client 在 NAT 留下的映射其他 IP 也能通过，因此 NAT 属于 EndPoint Independent 的 Filter 规则
		// 若连接失败，则从 IP1:Port2 对客户端外网地址发起连接，若连接成功，表示 Client 在 NAT 留下的映射只有同一个 IP 才能通过，
		// 因此 NAT 属于 Address Dependent 的 Filter 规则；若连接失败，表示相同 IP 不同 Port 的不能通过，
		// 因此 NAT 属于 Address And Port Denpendent 的Filter规则
        ClientSocket *c = ReuseSocketFactory::GetInstance()->GetClientSocket();
        if(!c->bind(m_another_addr,m_main_port)){
            delete c;
            return;
        }

        bool canConnect = c->connect(ext_ip,ext_port,_getConnectRetryTime());
        filter_type filterType;
        if(canConnect)
        {
            log("NatCheckerServer: ","filterType: ENDPOINT_INDEPENDENT");
            filterType = nat_type::ENDPOINT_INDEPENDENT;
        }
        else
        {
        	// 若无法连接上，则关闭原来的 Client Socket，重新打开一个，绑定地址为 IP1:Port2，并尝试连接
            if(!(c->reopen() && c->bind(m_main_addr,m_another_port))){
                delete c;
                return;
            }

            canConnect = c->connect(ext_ip,ext_port,_getConnectRetryTime());

            if(canConnect){
                filterType = nat_type::ADDRESS_DEPENDENT;
                log("NatCheckerServer: ","filterType: ADDRESS_DEPENDENT");
            }else{
                filterType = nat_type::ADDRESS_AND_PORT_DEPENDENT;
                log("NatCheckerServer: ","filterType: ADDRESS_AND_PORT_DEPENDENT");
            }
        }

        delete c;
        c = NULL;

		// 为接下来检测 NAT 的 MAP 类型做准备，先在 IP2:Port1 监听，通知 Client 连接，若这一次(第2次)连接所观察到的地址与上一次(第1次)相同
		// 表示 NAT 对 Client 发到任何地址的包都映射到同一个外网地址上，说明 NAT 属于 EndPoint Independent 的 Map 规则，
		// 若不相同，则监听 IP2:Port2 ，并通知 Client 连接到此地址，若这一次(第3次)连接所观察到的地址与第2次的端口相同，
		// 则表示 NAT 对 Client 发到同一个 IP 的包都映射到同一个外网地址上，说明 NAT 属于 Address Dependent 的 Map 规则，
		// 记录第3次连接的 Port 与第1次连接的 Port 的差值，此为端口变化的规律（这里存在两次观察只有一个差值无法判断规律是否正确的不足）
		// 并返回信息给 Client，通知其 NAT 的 Map 类型以及告诉它停止检测
		// 若这一次(第3次)连接所观察到的地址与第2次的端口不同，表示 NAT 对 Client 发到任意不同地址的包都映射到新的地址上
		// 说明 NAT 属于 Address And Port Dependent 的 Map 规则
		// 这时已经掌握了三个不同端口相同 IP 的地址了，根据差值判断端口变化规律，若两次差值相同，则确定此差值，并通知 Client
		// 若两次差值不同，则继续循环，让 Client 继续连接，直至这边观察到连续两次差值相同或循环次数已经达到阈值为止
		
        ServerSocket *s = ReuseSocketFactory::GetInstance()->GetServerSocket();
        if(!(s->bind(m_another_addr,m_main_port) &&
             s->listen(DEFAULT_LISTEN_NUM))){
            delete s;
            return;
        }

        data.clear();
        data.add(CONTINUE,true);
		data.add(FILTER_TYPE,filterType);
        data.add(EXTERN_IP,ext_ip);
        data.add(EXTERN_PORT,ext_port);
        data.add(CHANGE_IP,m_another_addr);
        data.add(CHANGE_PORT,m_main_port);
        
        proxy.write(data);		// STUN 服务器将信息发送给 Client，并等待 Client 连接

        c = s->accept();

        string ext_ip2 = c->getPeerAddr();
        port_type ext_port2 = c->getPeerPort();

        log("NatCheckerServer: ","ext_ip2: ",ext_ip2);
        log("NatCheckerServer: ","ext_port2: ",ext_port2);

        // 假设 NAT 只有一个对外 IP 或 同一个内网主机向外通信时肯定会转换到同一个 IP (也许会改变端口)
        if(ext_ip2 != ext_ip)
            log("NatCheckerServer: ","Warning: The NAT allocate the different global IP to the same host");

        if(ext_ip2 == ext_ip && ext_port2 == ext_port) // 第2次的外网地址与第1次的相同
        {
            data.clear();
            data.add(MAP_TYPE,nat_type::ENDPOINT_INDEPENDENT);
            data.add(CONTINUE,false);

            proxy.setSocket(c);
            proxy.write(data);
            
            delete c;
            c = NULL;

            // 端口增量为0
            nat_type natType(true,nat_type::ENDPOINT_INDEPENDENT,filterType);
            natType.setPrediction();
            record.setNatType(natType);
        }
        else	// 第2次的外网地址与第1次的不同，需要进一步判断是 Address Dependent 还是 Address And Port Dependent
        {
            data.clear();
            data.add(CONTINUE,true);
            data.add(CHANGE_IP,m_another_addr);
            data.add(CHANGE_PORT,m_another_port);

            if(!(s->reopen() &&
                 s->bind(m_another_addr,m_another_port) &&
                 s->listen(DEFAULT_LISTEN_NUM)))
                return;

            proxy.setSocket(c);
            proxy.write(data);

            delete c;
            c = s->accept();

            string ext_ip3 = c->getPeerAddr();
            port_type ext_port3 = c->getPeerPort();

            log("NatCheckerServer: ","ext_ip3: ",ext_ip3);
            log("NatCheckerServer: ","ext_port3: ",ext_port3);

            // 假设 NAT 只有一个对外 IP 或 同一个内网主机向外通信时肯定会转换到同一个 IP (也许会改变端口)
            if(ext_ip2 != ext_ip3)
                log("NatCheckerServer: ", "Warning: The NAT allocate the different global IP to the same host");

            if(ext_ip3 == ext_ip2 && ext_port3 == ext_port2) // 第3次的外网地址与第2次的相同
            {
                data.clear();
                data.add(MAP_TYPE,nat_type::ADDRESS_DEPENDENT);
                data.add(CONTINUE,false);

                proxy.setSocket(c);
                proxy.write(data);

                delete c;
                c = NULL;
                
                // 端口增量为 ext_port2 - ext_port
                nat_type natType(true,nat_type::ADDRESS_DEPENDENT,filterType);
                natType.setPrediction(true,ext_port2 - ext_port);
                record.setNatType(natType);
                // 第2次的外网与第1次的不同，第3次与第2次的相同，表示是地址相关的 Map 类型，记得更新 DataRecord 的外部地址，以最新的为准
                record.setExtAddress(Address(ext_ip2,ext_port2 - ext_port + ext_port2));
            }
            else // 第3次与第2次的端口不同，表示时 Address And Port Dependent
            {
            	int delta_pre = ext_port2 - ext_port;
            	int delta_cur = ext_port3 - ext_port2;
                size_t try_time = 0;
                port_type ext_port_pre = ext_port3;

                ip_type ext_ip_n;
                port_type ext_port_n;
            	
                while( (delta_pre != delta_cur || delta_cur + ext_port_pre >= 65535) && try_time++ < _getMaxTryTime()){
                    data.clear();
                    data.add(CONTINUE,true);
                    data.add(CHANGE_IP,m_another_addr);
                    data.add(CHANGE_PORT,m_another_port + (port_type)(try_time));

                    if(!(s->close() && s->open() &&
                            s->bind(m_another_addr,m_another_port + try_time) &&
                            s->listen(DEFAULT_LISTEN_NUM)))
                        return;

                    proxy.setSocket(c);
                    proxy.write(data);

                    delete c;
                    c = s->accept();

                    ext_ip_n = c->getPeerAddr();
                    ext_port_n = c->getPeerPort();

                    log("NatCheckerServer: ","ext_ip_n: ",ext_ip_n);
                    log("NatCheckerServer: ","ext_port_n: ",ext_port_n);

                    delta_pre = delta_cur;
                    delta_cur = ext_port_n - ext_port_pre;
                    ext_port_pre = ext_port_n;
                }
            	
                data.clear();
                data.add(MAP_TYPE,nat_type::ADDRESS_AND_PORT_DEPENDENT);
                data.add(CONTINUE,false);

                proxy.setSocket(c);
                proxy.write(data);

                delete c;
                c = NULL;

                nat_type natType(true,nat_type::ADDRESS_AND_PORT_DEPENDENT,filterType);
                if( try_time == _getMaxTryTime() ){ // 端口随机变化
                    natType.setPrediction(false);
                }else{  // 设置增量为 delta_cur;
                    natType.setPrediction(true,delta_cur);
                }
                record.setNatType(natType);
                record.setExtAddress(Address(ext_ip_n,ext_port_n + delta_cur));	// 记得更新 DataRecord 的外部地址，以最新的为准
            }
        }
        delete s;
        s = NULL;
    }
    //delete client;		// 注意这里不能把 STUN 服务器与 client 发过来的第一次连接给断掉，因为有些对称型 NAT 第一次连接是用内网的源端口
    //client = NULL;			，第二次开始才从一个随机值进行递增分配，如果把这第一次连接断掉了，NAT 可能会复用这个端口，端口猜测会无效

    CHECK_STATE_EXCEPTION(m_database->addRecord(record));
}

void NatCheckerServer::waitForClient(){
    if(!m_main_server->isListen() && !setListenNum(DEFAULT_LISTEN_NUM))
        THROW_EXCEPTION(InvalidOperationException,"bind or listen error");

    /*ClientSocket *client;
    while(client = m_main_server->accept()){
        handle_request(client);			// 目前的处理是不要并发执行，来一个处理一个，因为检测 NAT 类型的过程中会等待对方发起连接，如果并发会混，也许这个线程接受到的 socket 连接是另一个线程需要的
    }*/
    // 由于不能立即删了第一次连接的 client，故这里采用 I/O 多路转接的方式，能够监听到 client 断开连接的情况，由客户端主动断开

    DefaultServerSocket *m_socket = dynamic_cast<DefaultServerSocket*>(m_main_server);
    CHECK_STATE_EXCEPTION(m_socket);

    fd_set set;
    int ret,maxfd,fd;
    while(1)
    {
        FD_ZERO(&set);

        FD_SET(m_socket->_getfd(),&set);		// 把服务器 Socket 和所有接受到的客户端连接 Socket 都添加到 set 中
        maxfd = m_socket->_getfd();

        for(vector<DefaultClientSocket*>::size_type i=0;i<m_clientVec.size();++i){
            fd = m_clientVec[i]->_getfd();

            FD_SET(fd,&set);

            if(maxfd < fd)
                maxfd = fd;
        }

        ret = select(maxfd+1,&set,NULL,NULL,NULL);

        if(ret == -1)
        {
            if(errno == EINTR)
                log("NatCheckerServer: ","Warning : NatCheckerServer is interrupted at function select in NatCheckerServer::waitForClient");
            else
                THROW_EXCEPTION(ErrorStateException,"select error in NatCheckerServer::waitForClient");
        }else if(ret == 0)
        {
            THROW_EXCEPTION(ErrorStateException,"select timeout in NatTraversalServer::waitForClient");
        }else
        {
            // 如果服务器端可读，亦即有新客户端请求连接，则接收连接，为其服务，进行 NAT 类型检测
            if(FD_ISSET(m_socket->_getfd(),&set))
            {
                ClientSocket *client = m_socket->accept();

                log("NatCheckerServer: Client \"",client->getPeerAddr(),':',client->getPeerPort(),"\" connect");

                DefaultClientSocket *defaultSocket = dynamic_cast<DefaultClientSocket*>(client);
                CHECK_STATE_EXCEPTION(defaultSocket);

                m_clientVec.push_back(defaultSocket);

                handle_request(client);

                --ret;
            }

            // 遍历存储的客户端 socket 判断是否可读，若可读客户端断开连接
            vector<DefaultClientSocket*>::iterator it = m_clientVec.begin();
            while(it != m_clientVec.end() && ret != 0){
                if(FD_ISSET((*it)->_getfd(),&set))
                {
                    string content = (*it)->read();
                    CHECK_STATE_EXCEPTION(content.empty());

                    log("NatCheckerServer: Client \"",(*it)->getPeerAddr(),':',(*it)->getPeerPort(),"\" disconnect");

                    delete (*it);

                    --ret;
                    it = m_clientVec.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }
}
