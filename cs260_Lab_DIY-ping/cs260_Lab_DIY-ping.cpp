#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <vector>
#include <algorithm>
#include <numeric>
#include <string>
#include <stdexcept>
#include <memory>
#include <atomic>
#include <iomanip>

static std::atomic<bool> g_running(true);

BOOL WINAPI ConsoleHandler(DWORD signal) 
{
    // In case of while infinite pinging, pressing CTRL-C, it sets the running flag to false to stop the ping loop
    // To work as same as CMD ping, we should not exit the program
    if (signal == CTRL_C_EVENT) 
    {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}


class PingOptions {
public:
    int numPings = 4;
    int ttl = 128;
    int timeout = 1000;
    int dataSize = 32;
    bool infinitePing = false;
    std::string target;
    std::string targetIP;
    bool isDomain = false;

    PingOptions(int argc, char* argv[]) 
	{
        if (argc < 2) 
		{
            throw std::runtime_error("Usage: " + std::string(argv[0]) + " <IP Address or Hostname> [options]");
        }
        target = argv[1];
        parseOptions(argc, argv);
    }

private:
    void parseOptions(int argc, char* argv[]) 
	{
        for (int i = 2; i < argc; i++) 
		{
            std::string arg = argv[i];
            if (arg == "-t") 
			{
                infinitePing = true;
            }
            else if (arg == "-n" && i + 1 < argc) 
			{
                numPings = std::stoi(argv[++i]);
            }
            else if (arg == "-l" && i + 1 < argc) 
			{
                dataSize = std::stoi(argv[++i]);
            }
            else if (arg == "-i" && i + 1 < argc)
			{
                ttl = std::stoi(argv[++i]);
            }
            else if (arg == "-w" && i + 1 < argc) 
			{
                timeout = std::stoi(argv[++i]);
            }
        }
    }
};

class WinsockInitializer 
{
public:
    WinsockInitializer() 
	{
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) 
		{
            throw std::runtime_error("WSAStartup failed!");
        }
    }
    ~WinsockInitializer() 
	{
        WSACleanup();
    }
private:
    WSADATA wsaData;
};

class IcmpHandle 
{
public:
    IcmpHandle() : handle(IcmpCreateFile()) 
    {
        if (handle == INVALID_HANDLE_VALUE) 
        {
            throw std::runtime_error("IcmpCreateFile failed!");
        }
    }
    ~IcmpHandle() 
    {
        IcmpCloseHandle(handle);
    }

    HANDLE get() const { return handle; }
private:
    HANDLE handle;
};

class Pinger 
{
public:
    Pinger(const PingOptions& options) : options(options) 
    {
        resolveAddress();
        prepareData();
    }

    void run() 
    {
        if (!SetConsoleCtrlHandler((PHANDLER_ROUTINE)ConsoleHandler, TRUE)) 
        {
            throw std::runtime_error("Could not set control handler");
        }


        int packetsSent = 0, packetsReceived = 0;
        std::vector<DWORD> roundTripTimes;

        std::cout << "Pinging " << options.target.c_str() << " with " << options.dataSize << " bytes of data:" << std::endl;

        while (g_running && (options.infinitePing || packetsSent < options.numPings)) 
        {
            packetsSent++;
            auto result = sendPing();
            if (result.has_value()) 
            {
                packetsReceived++;
                roundTripTimes.push_back(result->RoundTripTime);
                printReply(*result);
            }
            else 
            {
                std::cerr << "Request timed out." << std::endl;
            }

            if (g_running && (options.infinitePing || packetsSent < options.numPings)) 
            {
                Sleep(1000);
            }
        }

        printStatistics(packetsSent, packetsReceived, roundTripTimes);
    }

private:
    PingOptions options;
    IcmpHandle icmpHandle;
    IN_ADDR targetAddress;
    std::unique_ptr<char[]> sendData;

    void resolveAddress() 
    {
        addrinfo hints = { 0 }, * res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(options.target.c_str(), nullptr, &hints, &res) != 0) 
        {
            throw std::runtime_error("getaddrinfo failed!");
        }
        
        sockaddr_in* ipv4 = reinterpret_cast<sockaddr_in*>(res->ai_addr);
        targetAddress = ipv4->sin_addr;
        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(res->ai_family, &ipv4->sin_addr, ipstr, INET_ADDRSTRLEN);
        options.targetIP = ipstr;
        
        if(options.targetIP == options.target) 
        {
			options.isDomain = false;
		}

        freeaddrinfo(res);
    }

    void prepareData() 
    {
        sendData = std::make_unique<char[]>(options.dataSize);
        std::fill_n(sendData.get(), options.dataSize, 'A');
    }

    std::optional<ICMP_ECHO_REPLY> sendPing() 
    {
        DWORD replySize = sizeof(ICMP_ECHO_REPLY) + options.dataSize;
        auto replyBuffer = std::make_unique<char[]>(replySize);
        auto reply = reinterpret_cast<ICMP_ECHO_REPLY*>(replyBuffer.get());

        DWORD result = IcmpSendEcho(icmpHandle.get(), targetAddress.S_un.S_addr,
            sendData.get(), options.dataSize, nullptr, reply, replySize, options.timeout);

        if (result != 0) 
        {
            return *reply;
        }

        return std::nullopt;
    }

    void printReply(const ICMP_ECHO_REPLY& reply) 
    {
        std::cout << "Reply from " << options.targetIP << ": bytes=" << reply.DataSize
            << " time=" << reply.RoundTripTime << "ms TTL=" << static_cast<int>(reply.Options.Ttl)
            << std::endl;
    }

    void printStatistics(int sent, int received, const std::vector<DWORD>& times) 
    {
        std::cout << "\nPing statistics for " << options.target << ":" << std::endl;
        std::cout << "    Packets: Sent = " << sent << ", Received = " << received
            << ", Lost = " << (sent - received)
            << " (" << std::fixed << std::setprecision(1) << 100.0 * (sent - received) / sent << "% loss)," << std::endl;

        if (!times.empty()) 
        {
            auto [min, max] = std::minmax_element(times.begin(), times.end());
            double avg = std::accumulate(times.begin(), times.end(), 0.0) / times.size();

            std::cout << "Approximate round trip times in milli-seconds:" << std::endl;
            std::cout << "    Minimum = " << *min << "ms, Maximum = " << *max
                << "ms, Average = " << static_cast<int>(std::round(avg)) << "ms" << std::endl;
        }
    }
};

int main(int argc, char* argv[]) 
{
    try 
    {
        WinsockInitializer winsockInit;
        PingOptions options(argc, argv);
        Pinger pinger(options);
        pinger.run();
    }
    catch (const std::exception& e) 
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
