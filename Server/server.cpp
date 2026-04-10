// server.cpp


#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

#include <windows.h>   // CreateThread, CRITICAL_SECTION

#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <regex>
#include <iomanip>
#include <ctime>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
static const int    DEFAULT_PORT = 27000;
static const int    BUFFER_SIZE = 128;
static const int    MAX_BACKLOG = 64;   // max queued datagrams (advisory)

// ─────────────────────────────────────────────────────────────────────────────
// Data structures
// ─────────────────────────────────────────────────────────────────────────────

// One telemetry sample received from a client
struct TelemData {
    string time;
    double fuel = 0.0;
};

// Per-plane flight session (lives only while a flight is active)
struct FlightSession {
    string   planeID;
    vector<TelemData> samples;    // all received data points this flight
    // Fuel consumption between consecutive samples is:
    //   consumption[i] = samples[i].fuel - samples[i+1].fuel
    // (positive when fuel is decreasing, which is the normal direction)
};

// Persistent record kept across multiple flights for a plane
struct PlaneRecord {
    string planeID;
    vector<double> flightAverages;  // one entry per completed flight
};

// Packet passed from the receiver thread to a worker thread
struct Packet {
    char    buf[BUFFER_SIZE];
    int     len;
    sockaddr_in senderAddr;
};

// ─────────────────────────────────────────────────────────────────────────────
// Global shared state  (protected by g_lock)
// ─────────────────────────────────────────────────────────────────────────────
static CRITICAL_SECTION g_lock;

// Active flights keyed by planeID
static unordered_map<string, FlightSession> g_activeSessions;

// All-time records keyed by planeID
static unordered_map<string, PlaneRecord> g_planeRecords;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Wall-clock timestamp for log messages
static string nowString() {
    time_t t = time(nullptr);
    char buf[32];
    struct tm localT;
    localtime_s(&localT, &t);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &localT);
    return string(buf);
}

static string trim(const string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Validate timestamp: D_M_YYYY HH:MM:SS  (matches client regex exactly)
static bool isValidTime(const string& input) {
    static const regex ts(
        R"(^([1-9]|[12][0-9]|3[01])_([1-9]|1[0-2])_(\d{4}) ([01][0-9]|2[0-3]):([0-5][0-9]):([0-5][0-9])$)"
    );
    return regex_match(input, ts);
}

// Validate that a string is a parseable double
static bool isValidDouble(const string& input) {
    try { stod(input); return true; }
    catch (...) { return false; }
}

// Parse "DAT:" payload → TelemData.  Returns false on bad format.
static bool parseTelemData(const string& payload, TelemData& out) {
    // payload = "timestamp,fuel[,optional trailing fields]"
    stringstream ss(payload);
    string token;
    vector<string> parts;
    while (getline(ss, token, ','))
        parts.push_back(trim(token));

    if (parts.size() < 2) return false;
    if (!isValidTime(parts[0])) return false;
    if (!isValidDouble(parts[1])) return false;

    out.time = parts[0];
    out.fuel = stod(parts[1]);
    return true;
}

// Calculate per-interval consumption values from a session's sample list
// Returns the average consumption per interval, or 0 if < 2 samples.
static double calcAverageConsumption(const FlightSession& session) {
    const auto& s = session.samples;
    if (s.size() < 2) return 0.0;

    double total = 0.0;
    int    count = 0;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        double delta = s[i].fuel - s[i + 1].fuel;  // positive = consumed
        total += delta;
        ++count;
    }
    return (count > 0) ? (total / count) : 0.0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Packet processing  (called from each worker thread)
// ─────────────────────────────────────────────────────────────────────────────

static void processPacket(const Packet& pkt) {
    // Null-terminate and convert buffer to string
    char safe[BUFFER_SIZE + 1] = {};
    memcpy(safe, pkt.buf, pkt.len < BUFFER_SIZE ? pkt.len : BUFFER_SIZE);
    string raw = trim(string(safe));

    if (raw.size() < 4) {
        cerr << "[" << nowString() << "] WARN: packet too short, ignored\n";
        return;
    }

    // Extract prefix (first 4 chars: "XXX:")
    string prefix = raw.substr(0, 4);
    string body = raw.size() > 4 ? raw.substr(4) : "";

    // ── CON: connection / identification ─────────────────────────────────────
    if (prefix == "CON:") {
        string planeID = trim(body);
        if (planeID.empty()) {
            cerr << "[" << nowString() << "] WARN: CON packet has empty planeID\n";
            return;
        }

        EnterCriticalSection(&g_lock);

        if (g_activeSessions.count(planeID)) {
            cout << "[" << nowString() << "] INFO: CON from already-active plane "
                << planeID << " — resetting session\n";
            g_activeSessions.erase(planeID);
        }

        FlightSession session;
        session.planeID = planeID;
        g_activeSessions[planeID] = session;

        // Ensure a PlaneRecord exists
        if (!g_planeRecords.count(planeID)) {
            PlaneRecord rec;
            rec.planeID = planeID;
            g_planeRecords[planeID] = rec;
        }

        LeaveCriticalSection(&g_lock);

        cout << "[" << nowString() << "] CONNECT  plane=" << planeID << "\n";
        return;
    }

    // ── DAT: telemetry data ───────────────────────────────────────────────────
    if (prefix == "DAT:") {
        TelemData td;
        if (!parseTelemData(body, td)) {
            cerr << "[" << nowString() << "] WARN: malformed DAT payload: " << body << "\n";
            return;
        }

        // We need to know which plane sent this.
        // The planeID is looked up by matching the sender's IP:port.
        // Since UDP is connectionless we match by the last CON sender address.
        // For simplicity here we search active sessions for a plane whose
        // most-recent sample matches — but the cleanest approach is to embed
        // planeID in every packet.  We therefore expect "DAT:<planeID>,<ts>,<fuel>".

        // Re-parse with planeID as first field: "planeID,timestamp,fuel"
        {
            stringstream ss2(body);
            string tok;
            vector<string> parts;
            while (getline(ss2, tok, ','))
                parts.push_back(trim(tok));

            // If 3+ fields: parts[0]=planeID, parts[1]=time, parts[2]=fuel
            // If 2 fields:  assume legacy format without planeID (log a warning)
            if (parts.size() >= 3) {
                string planeID = parts[0];
                string tsPart = parts[1];
                string fuelPart = parts[2];

                if (!isValidTime(tsPart) || !isValidDouble(fuelPart)) {
                    cerr << "[" << nowString() << "] WARN: invalid DAT fields for " << planeID << "\n";
                    return;
                }
                td.time = tsPart;
                td.fuel = stod(fuelPart);

                EnterCriticalSection(&g_lock);

                if (!g_activeSessions.count(planeID)) {
                    // Auto-create session (client may not have sent CON)
                    FlightSession s; s.planeID = planeID;
                    g_activeSessions[planeID] = s;
                    if (!g_planeRecords.count(planeID)) {
                        PlaneRecord r; r.planeID = planeID;
                        g_planeRecords[planeID] = r;
                    }
                    cout << "[" << nowString() << "] INFO: auto-created session for " << planeID << "\n";
                }

                g_activeSessions[planeID].samples.push_back(td);
                size_t nSamples = g_activeSessions[planeID].samples.size();

                LeaveCriticalSection(&g_lock);

                cout << "[" << nowString() << "] DATA     plane=" << planeID
                    << " time=" << td.time
                    << " fuel=" << fixed << setprecision(6) << td.fuel
                    << " (#" << nSamples << ")\n";
            }
            else {
                // Legacy 2-field DAT (no planeID) — log and skip
                cerr << "[" << nowString() << "] WARN: DAT packet missing planeID field — ignored\n";
            }
        }
        return;
    }

    // ── EOF: end of flight ────────────────────────────────────────────────────
    if (prefix == "EOF:") {
        string planeID = trim(body);
        if (planeID.empty()) {
            cerr << "[" << nowString() << "] WARN: EOF packet has empty planeID\n";
            return;
        }

        EnterCriticalSection(&g_lock);

        if (!g_activeSessions.count(planeID)) {
            LeaveCriticalSection(&g_lock);
            cerr << "[" << nowString() << "] WARN: EOF for unknown plane " << planeID << "\n";
            return;
        }

        FlightSession session = g_activeSessions[planeID];   // copy before erase
        g_activeSessions.erase(planeID);

        double avgConsumption = calcAverageConsumption(session);
        g_planeRecords[planeID].flightAverages.push_back(avgConsumption);

        // Compute all-time average across all flights
        const vector<double>& avgs = g_planeRecords[planeID].flightAverages;
        double allTimeAvg = 0.0;
        for (double v : avgs) allTimeAvg += v;
        allTimeAvg /= static_cast<double>(avgs.size());

        LeaveCriticalSection(&g_lock);

        cout << "[" << nowString() << "] EOF      plane=" << planeID
            << " samples=" << session.samples.size()
            << " flightAvgConsumption=" << fixed << setprecision(6) << avgConsumption
            << " allTimeAvg=" << allTimeAvg
            << " (over " << avgs.size() << " flight(s))\n";
        return;
    }

    // ── Unknown prefix ────────────────────────────────────────────────────────
    cerr << "[" << nowString() << "] WARN: unknown packet prefix '" << prefix
        << "' — raw: " << raw << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Worker thread  (one per received packet — thread-per-connection model)
// ─────────────────────────────────────────────────────────────────────────────

static DWORD WINAPI workerThread(LPVOID param) {
    Packet* pkt = reinterpret_cast<Packet*>(param);
    processPacket(*pkt);
    delete pkt;
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {

    // ── Argument parsing ──────────────────────────────────────────────────────
    int listenPort = DEFAULT_PORT;
    if (argc >= 2) {
        listenPort = atoi(argv[1]);
        if (listenPort <= 0 || listenPort > 65535) {
            cerr << "Usage: " << argv[0] << " [port]\n"
                << "  port must be 1-65535 (default " << DEFAULT_PORT << ")\n";
            return -1;
        }
    }

    // Initialise Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cerr << "ERROR: WSAStartup failed\n";
        return -1;
    }

    // Create UDP socket
    SOCKET serverSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (serverSock == INVALID_SOCKET) {
        cerr << "ERROR: socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return -1;
    }

    // Bind to all interfaces on listenPort (INADDR_ANY lets clients on any
    // network adapter reach the server — no IP is hardcoded here)
    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(static_cast<u_short>(listenPort));

    if (bind(serverSock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        cerr << "ERROR: bind() failed: " << WSAGetLastError() << "\n";
        closesocket(serverSock);
        WSACleanup();
        return -1;
    }

    // Initialise critical section for shared-state protection
    InitializeCriticalSection(&g_lock);

    cout << "[" << nowString() << "] Telemetry server listening on UDP port "
        << listenPort << " (all interfaces)\n";

    // ── Main receive loop ──────────────────────────────────────────────────────
    while (true) {
        Packet* pkt = new Packet{};
        int addrLen = sizeof(pkt->senderAddr);

        pkt->len = recvfrom(
            serverSock,
            pkt->buf,
            BUFFER_SIZE,
            0,
            reinterpret_cast<sockaddr*>(&pkt->senderAddr),
            &addrLen
        );

        if (pkt->len == SOCKET_ERROR) {
            cerr << "ERROR: recvfrom() failed: " << WSAGetLastError() << "\n";
            delete pkt;
            continue;   // keep running unless shutdown is requested
        }

        // Dispatch to a new worker thread (SYS-010: parallel thread design)
        HANDLE hThread = CreateThread(
            nullptr,        // default security
            0,              // default stack size
            workerThread,
            pkt,            // ownership transferred; worker deletes it
            0,
            nullptr
        );

        if (hThread == nullptr) {
            cerr << "ERROR: CreateThread failed: " << GetLastError() << "\n";
            // Fall back to inline processing so the packet isn't lost
            processPacket(*pkt);
            delete pkt;
        }
        else {
            // Detach — we don't need to wait on individual worker threads
            CloseHandle(hThread);
        }
    }

    // Cleanup (unreachable in this design; add a signal handler for graceful shutdown)
    DeleteCriticalSection(&g_lock);
    closesocket(serverSock);
    WSACleanup();
    return 0;
}