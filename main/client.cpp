#include "../include/natchecker/NatCheckerClient.h"

#include <iostream>

using namespace std;
using namespace Lib;

int main(int argc,char *argv[]){
	Object::port_type local_port = 8888;
	Object::port_type server_port = 8888;

	NatCheckerClient client("client","xxx.xxx.xxx.xxx",local_port);
	
	if(!client.connect("xxx.xxx.xxx.xxx",server_port)){
		cout << "can not connect to server" << endl;
		return 1;
	}
		
	nat_type natType = client.getNatType();
	
	cout << "has NAT: " << natType.haveNat() << endl;
	cout << natType.getStringMapType() << endl;
	cout << natType.getStringFilterType() << endl;

    return 0;
}
