#include <windows.networking.sockets.h>
#pragma comment(lib, "Ws2_32.lib")
#include <iostream>
#include <sstream>
#include <vector>

using namespace std;

// split a comma-delimited string
vector<string> splitLine(string line){
	char delimiter = ','; // should always be a comma in the files
	stringstream stream(line);
	string token;
	vector<string> result;

	while(getline(stream, token, delimiter)){
		result.push_back(token);
	}

	return result;
}

int main(int argc, char argv[]) {
	WSADATA wsaData;
	SOCKET ClientSocket;
	sockaddr_in SvrAddr;

	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		cerr << "ERROR: WSA Startup error" << endl;
		return -1;
	}

	// create UDP socket
	if ((ClientSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
		cerr << "ERROR: Failed to create UDP socket" << endl;
		return -1;
	}

	// set up sever address info
	// TODO: make address (and port?) configurable
	SvrAddr.sin_family = AF_INET;
	SvrAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
	SvrAddr.sin_port = htons(27000);

	// temp send test
	char TxBuffer[128] = { '1', '2', '3' };
	sendto(ClientSocket, TxBuffer, sizeof(TxBuffer), 0,
		(sockaddr*)&SvrAddr, sizeof(SvrAddr));
	cout << "Sent: " << TxBuffer << endl;

	// split line test
	string line = "FUEL TOTAL QUANTITY,D_M_YYYY HH:MM:SS,00.000000,";
	vector<string> result = splitLine(line);
	for (string word : result) {
		cout << "Word: " << word << endl;
	}

	// open file
	// read and process lines into data
	// send data packets to server until empty
	// send EOF packet

	// cleanup
	closesocket(ClientSocket);
	WSACleanup();
	return 0;
}