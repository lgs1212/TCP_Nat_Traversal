#include "../include/natchecker/NatCheckerServer.h"
#include "../database/DefaultDataBase.h"

using namespace Lib;

int main(){
    NatCheckerServer server("main_server_ip",8888,"secondary_server_ip",9999);

	DefaultDataBase<DataRecord> *database = new DefaultDataBase<DataRecord>();
	
	server.setDataBase(database);

	server.waitForClient();

	return 0;
}

