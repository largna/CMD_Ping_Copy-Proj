#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <vector>
#include <algorithm>
#include <numeric>
#include <atomic>
#include <iomanip>
#include <memory>
#include <string>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")

static std::atomic<bool> g_running(true);

BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT) 
    {
        // In case of while infinite pinging, pressing CTRL-C, it sets the running flag to false to stop the ping loop
        // To work as same as CMD ping, we should not exit the program
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char* argv[]) {
    // Default values
    int numPings = 4;
    int ttl = 128;
    int timeout = 1000;
    int dataSize = 32;
    bool infinitePing = false;

    // Handler setting
    if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE))
    {
        std::cerr << "Failed to set control handler" << std::endl;
        return 1;
    }

    // Option parsing
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <IP Address or Hostname> [options]" << std::endl;
        return 1;
    }

    std::string DomainName = argv[1];

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-t") == 0)
        {
            infinitePing = true;
        }
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
        {
            numPings = std::stoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            dataSize = std::stoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
        {
            ttl = std::stoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
        {
            timeout = std::stoi(argv[++i]);
        }
    }

    // Winsock Initialize
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "WSAStartup failed!" << std::endl;
        return 1;
    }

    // Create an ICMP handle
    HANDLE hIcmpFile = IcmpCreateFile();
    if (hIcmpFile == INVALID_HANDLE_VALUE)
    {
        std::cerr << "IcmpCreateFile failed!" << std::endl;
        WSACleanup();
        return 1;
    }

    // Convert the hostname to IP address
    addrinfo hints = { 0 };
    addrinfo* res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(argv[1], nullptr, &hints, &res) != 0)
    {
        std::cerr << "getaddrinfo failed!" << std::endl;
        IcmpCloseHandle(hIcmpFile);
        WSACleanup();
        return 1;
    }

    // To get the IP address from the result
    sockaddr_in* ipv4 = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    IN_ADDR targetAddress = ipv4->sin_addr;
    char ipstr[INET_ADDRSTRLEN];
    inet_ntop(res->ai_family, &ipv4->sin_addr, ipstr, INET_ADDRSTRLEN);
    std::string targetIP = ipstr;

    // Send data initialization
    std::vector<char> sendData(dataSize, 'A');

    int packetsSent = 0, packetsReceived = 0;
    std::vector<DWORD> roundTripTimes;

    if (DomainName == targetIP)
    {
        std::cout << std::endl << "Pinging " << targetIP << " with " << dataSize << " bytes of data:" << std::endl;
    }
    else
    {
        std::cout << std::endl << "Pinging " << DomainName << " [ " << targetIP << " ] " << " with " << dataSize << " bytes of data:" << std::endl;
    }

    // Ping loop
    while (g_running && (infinitePing || packetsSent < numPings))
    {
        packetsSent++;
        DWORD replySize = sizeof(ICMP_ECHO_REPLY) + dataSize + 8;
        auto replyBuffer = std::make_unique<char[]>(replySize);
        auto reply = reinterpret_cast<ICMP_ECHO_REPLY*>(replyBuffer.get());

        // Start Ping
        DWORD result = IcmpSendEcho(hIcmpFile, targetAddress.S_un.S_addr,
            sendData.data(), dataSize, nullptr, reply, replySize, timeout);

        if (result != 0)
        {
            packetsReceived++;
            roundTripTimes.push_back(reply->RoundTripTime);
            std::cout << "Reply from " << targetIP << ": bytes=" << reply->DataSize
                << " time=" << reply->RoundTripTime << "ms TTL=" << static_cast<int>(reply->Options.Ttl)
                << std::endl;
        }
        else
        {
            std::cerr << "Request timed out." << std::endl;
        }

        if (g_running && (infinitePing || packetsSent < numPings))
        {
            Sleep(1000);
        }
    }

    // Statistics Calculation && Output
    if (!roundTripTimes.empty())
    {
        DWORD minTime = *std::min_element(roundTripTimes.begin(), roundTripTimes.end());
        DWORD maxTime = *std::max_element(roundTripTimes.begin(), roundTripTimes.end());
        double avgTime = std::accumulate(roundTripTimes.begin(), roundTripTimes.end(), 0.0) / roundTripTimes.size();

        std::cout << std::endl << "Ping statistics for " << targetIP << ":" << std::endl;
        std::cout << "    Packets: Sent = " << packetsSent << ", Received = " << packetsReceived
            << ", Lost = " << (packetsSent - packetsReceived)
            << " (" << std::fixed << std::setprecision(0) << 100.0 * (packetsSent - packetsReceived) / packetsSent << "% loss)," << std::endl;
        std::cout << "Approximate round trip times in milli-seconds:" << std::endl;
        std::cout << "    Minimum = " << minTime << "ms, Maximum = " << maxTime
            << "ms, Average = " << std::fixed << std::setprecision(0) << avgTime << "ms" << std::endl;
    }
    else
    {
        std::cout << std::endl << "Ping statistics for " << targetIP << ":" << std::endl;
        std::cout << "    Packets: Sent = " << packetsSent << ", Received = 0, Lost = " << packetsSent
            << " (100% loss)," << std::endl;
    }

    // Resource Cleanup
    freeaddrinfo(res);
    IcmpCloseHandle(hIcmpFile);
    WSACleanup();

    return 0;
}