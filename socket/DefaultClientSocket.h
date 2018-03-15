#ifndef DEFAULT_CLIENT_SOCKET_H
#define DEFAULT_CLIENT_SOCKET_H

#include "../include/socket/ClientSocket.h"
#include "DefaultSocket.h"

LIB_BEGIN

#define DEFAULT_READ_BYTE 512

class DefaultClientSocket : public ClientSocket
{

public:
	DefaultClientSocket() :m_bHasConnect(false) { }
	explicit DefaultClientSocket(int socket);		// 本来想写成protected的，但是这样得申明DefaultServerSocket是它的友元，想想还是这么处理吧
    DefaultClientSocket(int socket,const std::string& addr,port_type port);

	virtual bool open() { return m_socket.open(); }
	virtual bool close();
	virtual bool isOpen() { return m_socket.isOpen(); }

	virtual bool bind(port_type port, const std::string& addr) { return m_socket.bind(port, addr); }
	virtual bool isBound() { return m_socket.isBound(); }

	virtual std::string read(int read_bype = DEFAULT_READ_BYTE);		// 默认读取 512 个字节
	virtual size_t write(const char*);

	virtual bool connect(const char*, port_type);
	virtual bool isConnected() { return m_bHasConnect; }

	virtual port_type getPort() { return m_socket._port(); }
	virtual std::string getAddr() { return m_socket._addr(); }

	virtual port_type getPeerPort();
	virtual std::string getPeerAddr();

protected:

	int _getMaxTryTime() { return 10; }

protected:
	DefaultSocket m_socket;

	bool m_bHasConnect;
};

LIB_END

#endif
