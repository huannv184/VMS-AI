#include "core/onvif_discovery.h"
#include "utils/logger.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <iostream>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <regex>
#include <random>

namespace vms {
namespace core {

// Generate a unique message ID for each probe
static std::string gen_probe_uuid() {
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 15);
    const char* hex = "0123456789abcdef";
    char uuid[37];
    for (int i = 0; i < 36; i++) uuid[i] = hex[dist(rng)];
    uuid[8] = '-'; uuid[13] = '-'; uuid[14] = '4';
    uuid[18] = '-'; uuid[19] = hex[8 + (dist(rng) % 4)];
    uuid[23] = '-'; uuid[36] = '\0';
    return std::string(uuid);
}

std::vector<OnvifDevice> OnvifDiscovery::discover(int timeout_ms) {
    LOG_INFO("Starting ONVIF WS-Discovery (multicast 239.255.255.250:3702)...");
    std::vector<OnvifDevice> devices;
    std::unordered_set<std::string> seen_ips;
    
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        LOG_ERROR("WSAStartup failed");
        return devices;
    }
#endif

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        LOG_ERROR("Failed to create UDP socket for ONVIF");
#ifdef _WIN32
        WSACleanup();
#endif
        return devices;
    }

    // Allow address reuse
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    // Set receive timeout
    DWORD recv_timeout = timeout_ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_timeout, sizeof(recv_timeout));

    // WS-Discovery multicast address (239.255.255.250:3702)
    sockaddr_in multicast_addr;
    multicast_addr.sin_family = AF_INET;
    multicast_addr.sin_port = htons(3702);
    inet_pton(AF_INET, "239.255.255.250", &multicast_addr.sin_addr);

    // Send probe 3 times (some cameras miss single probes)
    for (int retry = 0; retry < 3; retry++) {
        std::string msg_uuid = gen_probe_uuid();
        
        std::string probe_msg = 
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
            "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
            "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">\n"
            "  <e:Header>\n"
            "    <w:MessageID>uuid:" + msg_uuid + "</w:MessageID>\n"
            "    <w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>\n"
            "    <w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>\n"
            "  </e:Header>\n"
            "  <e:Body>\n"
            "    <d:Probe>\n"
            "      <d:Types>dn:NetworkVideoTransmitter</d:Types>\n"
            "    </d:Probe>\n"
            "  </e:Body>\n"
            "</e:Envelope>";

        int sendResult = sendto(sock, probe_msg.c_str(), (int)probe_msg.length(), 0, 
                                (sockaddr*)&multicast_addr, sizeof(multicast_addr));
        if (sendResult == SOCKET_ERROR) {
            LOG_ERROR("Failed to send ONVIF probe (retry {})", retry);
        } else {
            LOG_INFO("ONVIF probe #{} sent", retry + 1);
        }
        
        // Wait between retries
        if (retry < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
    }
    
    LOG_INFO("ONVIF probes sent. Waiting for responses...");
    
    // Receive responses
    char recvBuf[8192];
    sockaddr_in senderAddr;
    socklen_t senderAddrSize = sizeof(senderAddr);
    
    auto start_time = std::chrono::steady_clock::now();
    
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start_time).count() < timeout_ms) {
        int bytesReceived = recvfrom(sock, recvBuf, sizeof(recvBuf) - 1, 0, 
                                     (sockaddr*)&senderAddr, &senderAddrSize);
        if (bytesReceived > 0) {
            recvBuf[bytesReceived] = '\0';
            std::string response(recvBuf);
            
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &senderAddr.sin_addr, ip_str, INET_ADDRSTRLEN);
            std::string ip(ip_str);
            
            // Skip duplicates
            if (seen_ips.find(ip) != seen_ips.end()) continue;
            seen_ips.insert(ip);
            
            // Parse XAddrs
            std::string xaddrs;
            size_t xaddrs_start = response.find("XAddrs>");
            if (xaddrs_start != std::string::npos) {
                xaddrs_start += 7;
                size_t xaddrs_end = response.find("</", xaddrs_start);
                if (xaddrs_end != std::string::npos) {
                    xaddrs = response.substr(xaddrs_start, xaddrs_end - xaddrs_start);
                }
            }

            // Parse Scopes
            std::string scopes;
            size_t scopes_start = response.find("Scopes>");
            if (scopes_start != std::string::npos) {
                scopes_start += 7;
                size_t scopes_end = response.find("</", scopes_start);
                if (scopes_end != std::string::npos) {
                    scopes = response.substr(scopes_start, scopes_end - scopes_start);
                }
            }
            
            OnvifDevice dev;
            dev.ip = ip;
            dev.service_url = xaddrs.empty() ? ("http://" + ip + "/onvif/device_service") : xaddrs;
            dev.name = "ONVIF Camera (" + ip + ")";
            
            if (!scopes.empty()) {
                std::string s_lower = scopes;
                for (auto& c : s_lower) c = tolower(c);
                
                // Extract Name from /name/ scope
                std::regex name_re("/name/([^\\s]+)");
                std::smatch nm;
                if (std::regex_search(scopes, nm, name_re)) {
                    dev.name = nm[1].str();
                }
                
                // Extract Model from /hardware/ scope
                std::regex hw_re("/hardware/([^\\s]+)");
                std::smatch hm;
                if (std::regex_search(scopes, hm, hw_re)) {
                    dev.model = hm[1].str();
                }
                
                // Detect Manufacturer (expanded detection)
                if      (s_lower.find("hikvision") != std::string::npos || s_lower.find("hik-") != std::string::npos) dev.manufacturer = "Hikvision";
                else if (s_lower.find("dahua") != std::string::npos)     dev.manufacturer = "Dahua";
                else if (s_lower.find("axis") != std::string::npos)      dev.manufacturer = "Axis";
                else if (s_lower.find("bosch") != std::string::npos)     dev.manufacturer = "Bosch";
                else if (s_lower.find("hanwha") != std::string::npos || s_lower.find("samsung") != std::string::npos || s_lower.find("wisenet") != std::string::npos)
                    dev.manufacturer = "Hanwha";
                else if (s_lower.find("uniview") != std::string::npos || s_lower.find("unv") != std::string::npos) dev.manufacturer = "Uniview";
                else if (s_lower.find("reolink") != std::string::npos)   dev.manufacturer = "Reolink";
                else if (s_lower.find("milesight") != std::string::npos) dev.manufacturer = "Milesight";
                else if (s_lower.find("pelco") != std::string::npos)     dev.manufacturer = "Pelco";
                else if (s_lower.find("vivotek") != std::string::npos)   dev.manufacturer = "Vivotek";
                else if (s_lower.find("kbvision") != std::string::npos || s_lower.find("kbv") != std::string::npos) dev.manufacturer = "KBVision";
                else if (s_lower.find("imou") != std::string::npos)      dev.manufacturer = "Imou";
                else if (s_lower.find("ezviz") != std::string::npos)     dev.manufacturer = "Ezviz";
                else dev.manufacturer = "Generic ONVIF";
            }

            devices.push_back(dev);
            LOG_INFO("Discovered ONVIF device: {} (Brand: {}, Model: {})", ip, dev.manufacturer, dev.model);
        } else {
            // Timeout or error
            break;
        }
    }

#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#endif

    LOG_INFO("ONVIF discovery completed. Found {} devices.", devices.size());
    return devices;
}

} // namespace core
} // namespace vms
