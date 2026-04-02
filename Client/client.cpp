#include <windows.networking.sockets.h>
#pragma comment(lib, "Ws2_32.lib")
#include <iostream>
#include <sstream>
#include <vector>
#include <regex>

using namespace std;

// format for telemetry data extracted from a file
struct TelemData {
	string time;
	double fuel = 0.0;
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

	// split line & create data test
	string line1 = "FUEL TOTAL QUANTITY,1_1_2000 01:01:01,20.050000, ";
	string line2 = " 12_10_2025 10:23:42,11.203000, ";

	vector<string> vec1 = splitLine(line1);
	vector<string> vec2 = splitLine(line2);

	TelemData data1;
	TelemData data2;

	if (createTelemData(vec1, data1)) {
		cout << "Data 1:" << endl;
		cout << "Time: " << data1.time << " Fuel: " << data1.fuel << endl;
	}
	if (createTelemData(vec2, data2)) {
		cout << "Data 2:" << endl;
		cout << "Time: " << data2.time << " Fuel: " << data2.fuel << endl;
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