#define _CRT_SECURE_NO_WARNINGS
#include <windows.networking.sockets.h>
#pragma comment(lib, "Ws2_32.lib")
#include <iostream>
#include <sstream>
#include <vector>
#include <regex>
#include <string.h>
#include <fstream>

using namespace std;

const int PACKET_SIZE = 128;
const int PLANE_ID = 0; // change later

enum PacketType {
	TELEM,
	END,
	INVALID
};

// format for telemetry data extracted from a file
struct TelemData {
	string time;
	double fuel = 0.0;
};

// format for telemetry packet
struct TelemPacket {
	int planeID = 0;
	string timestamp;
	double fuel = 0.0;
	PacketType type = INVALID;
};

// remove leading and trailing whitespaces, if any
string trim(string& line) {
	size_t strBegin = line.find_first_not_of(" ");
	size_t strEnd = line.find_last_not_of(" ");
	size_t strRange = strEnd - strBegin + 1;

	return line.substr(strBegin, strRange);
}

// split a comma-delimited string
vector<string> splitLine(string line){
	line = trim(line);

	char delimiter = ','; // should always be a comma in the files
	stringstream stream(line);
	string token;
	vector<string> result;

	while(getline(stream, token, delimiter)){
		result.push_back(token);
	}

	return result;
}

// validate a string matches time format (D_M_YYYY HH:MM:SS)
bool isValidTime(string input) {
	regex timestamp(
		R"(^([1-9]|[12][0-9]|3[01])_([1-9]|1[0-2])_(\d{4}) ([01][0-9]|2[0-3]):([0-5][0-9]):([0-5][0-9])$)"
	);
	return regex_match(input, timestamp);
}

// validate a string can be converted into a double
bool isValidDouble(string input) {
	bool valid = true;

	try {
		double testDouble = stod(input);
	}
	catch (invalid_argument) {
		valid = false;
	}
	return valid;
}

// validate a split line has proper data
bool isValidData(vector<string> &input){
	// remove extra data from first line of file
	if (input.size() == 3) {
		input.erase(input.begin());
	}
	else if(input.size() != 2) {
		cout << "Invalid no. of data items" << endl;
		return false;
	}

	// validate vector contents
	if (!isValidTime(input.at(0))) {
		cout << "Invalid timestamp format" << endl;
		return false;
	}
	if (!isValidDouble(input.at(1))) {
		cout << "Invalid fuel format" << endl;
		return false;
	}

	return true;
}

// convert split line into telemetry data, if it is valid
bool createTelemData(vector<string> input, TelemData &output) {
	if (!isValidData(input)) {
		return false;
	}

	output.time = input.at(0);
	output.fuel = stod(input.at(1));
	return true;
}

// convert telemetry data into a packet format
TelemPacket createPacket(TelemData input){
	TelemPacket packet;

	packet.planeID = PLANE_ID;
	packet.fuel = input.fuel;
	packet.timestamp = input.time;
	if (packet.fuel == -1) {
		packet.type = END;
	}
	else {
		packet.type = TELEM;
	}

	return packet;
}

// Pack together telemetry data into a sendable format
void serializePacket(TelemPacket input, char output[PACKET_SIZE]) {
	string packetStr;

	switch (input.type) {
	case TELEM:
		packetStr = "DAT:";
		packetStr += to_string(input.planeID) + ",";
		packetStr += input.timestamp + ",";
		packetStr += to_string(input.fuel);
		break;
	case END:
		packetStr = "EOF:";
		packetStr += to_string(input.planeID);
		break;
	default:
		packetStr = "BAD:";
		break;
	}

	strcpy(output, packetStr.c_str());
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

	// read in file and send packets until done
	string filename;
	std::cout << "Enter the File path: ";
	getline(std::cin, filename);

	string enteredip;
	std::cout << "Enter the Host Server IP to begin: ";
	std::cin >> enteredip;

	// set up sever address info
	// TODO: make address (and port?) configurable
	SvrAddr.sin_family = AF_INET;
	SvrAddr.sin_addr.s_addr = inet_addr(enteredip.c_str());
	SvrAddr.sin_port = htons(27000);

	string line;
	ifstream file(filename);

	while (getline(file, line)) {
		vector<string> split = splitLine(line);
		TelemData data;

		if (createTelemData(split, data)) {
			cout << "Data:" << endl;
			cout << "Time: " << data.time << " Fuel: " << data.fuel << endl;
		}

		TelemPacket packet = createPacket(data);

		char buf[PACKET_SIZE];
		serializePacket(packet, buf);

		sendto(ClientSocket, buf, sizeof(buf), 0,
			(sockaddr*)&SvrAddr, sizeof(SvrAddr));
		cout << "Sent: " << buf << endl;
	}
	file.close();

	// create and send EOF packet
	TelemPacket endPkt;
	endPkt.fuel = -1;
	endPkt.timestamp = "";
	endPkt.planeID = PLANE_ID;
	endPkt.type = END;

	char endBuf[PACKET_SIZE];
	serializePacket(endPkt, endBuf);

	sendto(ClientSocket, endBuf, sizeof(endBuf), 0,
		(sockaddr*)&SvrAddr, sizeof(SvrAddr));
	cout << "Sent: " << endBuf << endl;

	// cleanup
	closesocket(ClientSocket);
	WSACleanup();
	return 0;
}