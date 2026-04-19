#include "core/network_scanner.h"
#include "utils/logger.h"
#include <iostream>
#include <random>
#include <iomanip>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <future>

#include <pugixml.hpp>
#include <curl/curl.h>
#include <boost/asio.hpp>
#include <regex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace vms {
namespace core {

NetworkScanner& NetworkScanner::getInstance() {
    static NetworkScanner instance;
    return instance;
}

NetworkScanner::NetworkScanner() {}

NetworkScanner::~NetworkScanner() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& pair : sessions_) {
        if (pair.second->status == "running") {
            pair.second->stop_flag = true;
            if (pair.second->worker.joinable()) {
                pair.second->worker.join();
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  UUID Generator
// ═══════════════════════════════════════════════════════════════
static std::string generate_uuid() {
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

// ═══════════════════════════════════════════════════════════════
//  Session Management
// ═══════════════════════════════════════════════════════════════
std::string NetworkScanner::startScan(const ScanConfig& config) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    std::string scan_id = generate_uuid();
    auto session = std::make_shared<ScanSession>();
    session->id = scan_id;
    session->config = config;
    session->status = "running";
    session->phase = "init";
    session->progress = 0;
    session->start_time = std::chrono::system_clock::now();
    session->stop_flag = false;

    session->worker = std::thread(&NetworkScanner::workerLoop, this, session);
    
    sessions_[scan_id] = session;
    return scan_id;
}

void NetworkScanner::stopScan(const std::string& scan_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(scan_id);
    if (it != sessions_.end()) {
        auto& session = it->second;
        if (session->status == "running") {
            session->stop_flag = true;
            session->status = "stopped";
        }
    }
}

nlohmann::json NetworkScanner::getScanStatus(const std::string& scan_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(scan_id);
    if (it == sessions_.end()) return {{"error", "Scan ID not found"}};
    
    auto& session = it->second;
    size_t dev_count;
    {
        std::lock_guard<std::mutex> dlock(session->devices_mutex);
        dev_count = session->devices.size();
    }
    return {
        {"id", session->id},
        {"status", session->status},
        {"phase", session->phase},
        {"progress", session->progress},
        {"devices_found", dev_count}
    };
}

nlohmann::json NetworkScanner::getScanResults(const std::string& scan_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(scan_id);
    if (it == sessions_.end()) return {{"error", "Scan ID not found"}};
    
    auto& session = it->second;
    nlohmann::json devices_json = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> dlock(session->devices_mutex);
        for (const auto& d : session->devices) {
            devices_json.push_back(d.to_json());
        }
    }
    
    return {
        {"id", session->id},
        {"status", session->status},
        {"phase", session->phase},
        {"results", devices_json}
    };
}

// ═══════════════════════════════════════════════════════════════
//  Device Cache (In-Memory, TTL 5 min)
// ═══════════════════════════════════════════════════════════════
void NetworkScanner::pruneExpiredCache() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = device_cache_.begin(); it != device_cache_.end(); ) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.discovered_at).count();
        if (age > CACHE_TTL_SECONDS) {
            it = device_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

bool NetworkScanner::isCached(const std::string& ip) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    pruneExpiredCache();
    return device_cache_.find(ip) != device_cache_.end();
}

void NetworkScanner::addToCache(const DeviceInfo& dev) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    CachedDevice cd;
    cd.info = dev;
    cd.discovered_at = std::chrono::steady_clock::now();
    device_cache_[dev.ip] = cd;
}

std::vector<DeviceInfo> NetworkScanner::getCachedDevices() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    pruneExpiredCache();
    std::vector<DeviceInfo> result;
    for (const auto& pair : device_cache_) {
        auto info = pair.second.info;
        info.discovery_method = "cache";
        result.push_back(info);
    }
    return result;
}

nlohmann::json NetworkScanner::getCachedDevicesJson() {
    auto devices = getCachedDevices();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& d : devices) {
        arr.push_back(d.to_json());
    }
    return {
        {"count", devices.size()},
        {"ttl_seconds", CACHE_TTL_SECONDS},
        {"devices", arr}
    };
}

void NetworkScanner::clearCache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    device_cache_.clear();
    LOG_INFO("Device cache cleared");
}

size_t NetworkScanner::getCacheSize() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    pruneExpiredCache();
    return device_cache_.size();
}

// ═══════════════════════════════════════════════════════════════
//  Helper: Thread-safe device operations
// ═══════════════════════════════════════════════════════════════
bool NetworkScanner::isAlreadyDiscovered(std::shared_ptr<ScanSession> session, const std::string& ip) {
    std::lock_guard<std::mutex> dlock(session->devices_mutex);
    for (const auto& d : session->devices) {
        if (d.ip == ip) return true;
    }
    return false;
}

void NetworkScanner::addDevice(std::shared_ptr<ScanSession> session, const DeviceInfo& dev) {
    {
        std::lock_guard<std::mutex> dlock(session->devices_mutex);
        // Double-check to avoid duplicates
        for (const auto& d : session->devices) {
            if (d.ip == dev.ip && d.port == dev.port) return;
        }
        session->devices.push_back(dev);
    }
    // Also add to global cache
    addToCache(dev);
    LOG_INFO("Device added: {} ({}) via {}", dev.ip, dev.vendor, dev.discovery_method);
}

// ═══════════════════════════════════════════════════════════════
//  Port Check (Boost::Asio, 150ms timeout)
// ═══════════════════════════════════════════════════════════════
bool NetworkScanner::checkPort(const std::string& ip, int port) {
    try {
        boost::asio::io_context io_context;
        boost::asio::ip::tcp::socket tcp_socket(io_context);
        boost::system::error_code ec;
        boost::asio::ip::address addr = boost::asio::ip::make_address(ip, ec);
        if (ec) return false;
        
        boost::asio::ip::tcp::endpoint endpoint(addr, port);
        
        bool connected = false;
        tcp_socket.async_connect(endpoint, [&](const boost::system::error_code& error) {
            if (!error) connected = true;
        });

        io_context.run_for(std::chrono::milliseconds(150));
        
        if (connected) {
            tcp_socket.close();
        }
        return connected;
    } catch (...) {
        return false;
    }
}

// ═══════════════════════════════════════════════════════════════
//  ONVIF Stream URI Fetch (via SOAP)
// ═══════════════════════════════════════════════════════════════
static size_t curl_string_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    if (userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
    }
    return size * nmemb;
}

bool NetworkScanner::fetchOnvifStreamUri(const std::string& ip, int port, const std::string& user, const std::string& pass, DeviceInfo& result) {
    std::string device_service_url = "http://" + ip + ":" + std::to_string(port) + "/onvif/device_service";
    
    CURL *curl;
    CURLcode res;
    std::string response_string;
    
    // Step 1: GetProfiles
    std::string get_profiles_xml = R"(
        <s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:trt="http://www.onvif.org/ver10/media/wsdl">
          <s:Body>
            <trt:GetProfiles/>
          </s:Body>
        </s:Envelope>
    )";

    curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/soap+xml; charset=utf-8; action=\"http://www.onvif.org/ver10/media/wsdl/GetProfiles\"");
    
    curl_easy_setopt(curl, CURLOPT_URL, device_service_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, get_profiles_xml.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_string_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
    
    if (!user.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
        std::string userpwd = user + ":" + pass;
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
    }

    res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return false;

    // Find first profile token
    std::string profile_token;
    pugi::xml_document doc;
    if (doc.load_string(response_string.c_str())) {
        for (auto node : doc.child("env:Envelope").child("env:Body").child("trt:GetProfilesResponse").children("trt:Profiles")) {
            profile_token = node.attribute("token").value();
            break;
        }
    }
    
    if (profile_token.empty()) {
        std::regex token_regex("token=\"([^\"]*)\"");
        std::smatch match;
        if (std::regex_search(response_string, match, token_regex) && match.size() > 1) {
            profile_token = match.str(1);
        }
    }
    
    if (profile_token.empty()) return false;

    // Step 2: GetStreamUri
    std::string get_stream_uri_xml = R"(
        <s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:trt="http://www.onvif.org/ver10/media/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema">
          <s:Body>
            <trt:GetStreamUri>
              <trt:StreamSetup>
                <tt:Stream>RTP-Unicast</tt:Stream>
                <tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>
              </trt:StreamSetup>
              <trt:ProfileToken>)" + profile_token + R"(</trt:ProfileToken>
            </trt:GetStreamUri>
          </s:Body>
        </s:Envelope>
    )";

    response_string.clear();
    curl = curl_easy_init();
    if (!curl) return false;

    headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/soap+xml; charset=utf-8; action=\"http://www.onvif.org/ver10/media/wsdl/GetStreamUri\"");
    
    curl_easy_setopt(curl, CURLOPT_URL, device_service_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, get_stream_uri_xml.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_string_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
    
    if (!user.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
        std::string userpwd = user + ":" + pass;
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
    }

    res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return false;

    std::regex uri_regex(R"(<tt:Uri>(.+?)</tt:Uri>)");
    std::smatch match;
    if (std::regex_search(response_string, match, uri_regex) && match.size() > 1) {
        std::string uri = match.str(1);
        if (uri.find("rtsp://") == 0 && uri.find("@") == std::string::npos && !user.empty()) {
            uri.insert(7, user + ":" + pass + "@");
        }
        result.rtsp_url = uri;
        result.is_auth_success = true;
        result.vendor = "onvif_device";
        result.username = user;
        result.password = pass;
        return true;
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════
//  Phase 1a: ONVIF WS-Discovery (UDP multicast 239.255.255.250:3702)
// ═══════════════════════════════════════════════════════════════
void NetworkScanner::performWSDiscovery(std::shared_ptr<ScanSession> session) {
    LOG_INFO("Phase 1a: ONVIF WS-Discovery starting...");
    session->phase = "onvif";
    
    try {
        boost::asio::io_context io_context;
        boost::asio::ip::udp::socket udp_socket(io_context);
        udp_socket.open(boost::asio::ip::udp::v4());
        udp_socket.set_option(boost::asio::ip::udp::socket::reuse_address(true));
        
        // Bind to any address so we can receive responses
        udp_socket.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));

        std::string msg_uuid = generate_uuid();
        std::string probe_msg = R"(<?xml version="1.0" encoding="UTF-8"?>
<e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope" xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery" xmlns:dn="http://www.onvif.org/ver10/network/wsdl">
  <e:Header>
    <w:MessageID>urn:uuid:)" + msg_uuid + R"(</w:MessageID>
    <w:To e:mustUnderstand="true">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>
    <w:Action a:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>
  </e:Header>
  <e:Body>
    <d:Probe>
      <d:Types>dn:NetworkVideoTransmitter</d:Types>
    </d:Probe>
  </e:Body>
</e:Envelope>)";

        // Send to multicast address (correct ONVIF standard)
        boost::asio::ip::udp::endpoint multicast_endpoint(
            boost::asio::ip::make_address("239.255.255.250"), 3702);

        // Send probe 3 times for reliability (some cameras are slow to respond)
        for (int retry = 0; retry < 3 && !session->stop_flag; retry++) {
            udp_socket.send_to(boost::asio::buffer(probe_msg), multicast_endpoint);
            if (retry < 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
        }

        LOG_INFO("ONVIF probe sent (3x). Listening for responses...");

        udp_socket.non_blocking(true);
        auto start_time = std::chrono::steady_clock::now();
        char recv_buffer[8192];
        boost::asio::ip::udp::endpoint sender_endpoint;
        
        std::unordered_set<std::string> discovered_ips;

        // Listen for 4 seconds
        while (std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - start_time).count() < 4) {
            if (session->stop_flag) break;
            
            boost::system::error_code ec;
            size_t len = udp_socket.receive_from(
                boost::asio::buffer(recv_buffer, sizeof(recv_buffer) - 1), 
                sender_endpoint, 0, ec);
            
            if (!ec && len > 0) {
                recv_buffer[len] = '\0';
                std::string ip = sender_endpoint.address().to_string();
                
                if (discovered_ips.find(ip) != discovered_ips.end()) continue;
                discovered_ips.insert(ip);
                
                std::string response(recv_buffer, len);
                
                DeviceInfo info;
                info.ip = ip;
                info.port = 80;
                info.discovery_method = "onvif";
                
                // Parse XAddrs for service URL
                size_t xaddrs_start = response.find("XAddrs>");
                if (xaddrs_start != std::string::npos) {
                    xaddrs_start += 7;
                    size_t xaddrs_end = response.find("</", xaddrs_start);
                    if (xaddrs_end != std::string::npos) {
                        std::string xaddrs = response.substr(xaddrs_start, xaddrs_end - xaddrs_start);
                        // Extract port from XAddrs if present
                        std::regex port_regex(R"(http[s]?://[^:]+:(\d+))");
                        std::smatch pm;
                        if (std::regex_search(xaddrs, pm, port_regex)) {
                            try { info.port = std::stoi(pm[1].str()); } catch (...) {}
                        }
                    }
                }

                // Parse Scopes for brand/model
                size_t scopes_start = response.find("Scopes>");
                if (scopes_start != std::string::npos) {
                    scopes_start += 7;
                    size_t scopes_end = response.find("</", scopes_start);
                    if (scopes_end != std::string::npos) {
                        std::string scopes = response.substr(scopes_start, scopes_end - scopes_start);
                        std::string s_lower = scopes;
                        for (auto& c : s_lower) c = tolower(c);
                        
                        // Detect vendor
                        if (s_lower.find("hikvision") != std::string::npos || s_lower.find("hik-") != std::string::npos)
                            info.vendor = "hikvision";
                        else if (s_lower.find("dahua") != std::string::npos)
                            info.vendor = "dahua";
                        else if (s_lower.find("axis") != std::string::npos)
                            info.vendor = "axis";
                        else if (s_lower.find("bosch") != std::string::npos)
                            info.vendor = "bosch";
                        else if (s_lower.find("hanwha") != std::string::npos || s_lower.find("samsung") != std::string::npos)
                            info.vendor = "hanwha";
                        else if (s_lower.find("uniview") != std::string::npos || s_lower.find("unv") != std::string::npos)
                            info.vendor = "uniview";
                        else if (s_lower.find("reolink") != std::string::npos)
                            info.vendor = "reolink";
                        else if (s_lower.find("milesight") != std::string::npos)
                            info.vendor = "milesight";
                        else
                            info.vendor = "onvif_generic";
                        
                        // Extract model from /hardware/ scope
                        std::regex hw_regex(R"(/hardware/([^\s]+))");
                        std::smatch hw_match;
                        if (std::regex_search(scopes, hw_match, hw_regex)) {
                            info.model = hw_match[1].str();
                        }
                        
                        // Extract name from /name/ scope
                        std::regex name_regex(R"(/name/([^\s]+))");
                        std::smatch name_match;
                        if (std::regex_search(scopes, name_match, name_regex)) {
                            if (info.model.empty()) info.model = name_match[1].str();
                        }
                    }
                }
                
                // Build default RTSP URL based on vendor
                if (info.vendor == "hikvision")
                    info.rtsp_url = "rtsp://" + ip + ":554/Streaming/Channels/101";
                else if (info.vendor == "dahua")
                    info.rtsp_url = "rtsp://" + ip + ":554/cam/realmonitor?channel=1&subtype=0";
                else if (info.vendor == "uniview")
                    info.rtsp_url = "rtsp://" + ip + ":554/video1";
                else if (info.vendor == "hanwha")
                    info.rtsp_url = "rtsp://" + ip + ":554/profile1/media.smp";
                else if (info.vendor == "reolink")
                    info.rtsp_url = "rtsp://" + ip + ":554/h264Preview_01_main";
                else
                    info.rtsp_url = "rtsp://" + ip + ":554/stream";
                
                info.is_auth_success = false;
                addDevice(session, info);
                LOG_INFO("ONVIF discovered: {} ({})", ip, info.vendor);
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        LOG_INFO("ONVIF WS-Discovery complete. Found {} devices via ONVIF.", discovered_ips.size());
    } catch (const std::exception& e) {
        LOG_ERROR("ONVIF WS-Discovery error: {}", e.what());
    } catch (...) {
        LOG_ERROR("ONVIF WS-Discovery unknown error");
    }
}

// ═══════════════════════════════════════════════════════════════
//  Phase 1b: SUNAPI Discovery (UDP broadcast :7701) — Hanwha/Samsung
// ═══════════════════════════════════════════════════════════════
void NetworkScanner::performSunapiDiscovery(std::shared_ptr<ScanSession> session) {
    LOG_INFO("Phase 1b: SUNAPI Discovery (UDP 7701) starting...");
    session->phase = "sunapi";
    
    try {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            LOG_ERROR("SUNAPI: WSAStartup failed");
            return;
        }
#endif

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            LOG_ERROR("SUNAPI: Failed to create UDP socket");
#ifdef _WIN32
            WSACleanup();
#endif
            return;
        }

        // Enable broadcast
        char broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        
        // Set receive timeout: 2 seconds
        DWORD timeout = 2000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

        // SUNAPI discovery packet (simple identify request)
        // Hanwha cameras listen on UDP port 7701 for discovery
        // The packet format is a simple "DISCOVER" text command
        std::string discover_pkt = "DISCOVER";
        
        sockaddr_in dest;
        dest.sin_family = AF_INET;
        dest.sin_port = htons(7701);
        dest.sin_addr.s_addr = INADDR_BROADCAST;
        
        // Send discovery broadcast
        sendto(sock, discover_pkt.c_str(), (int)discover_pkt.length(), 0,
               (sockaddr*)&dest, sizeof(dest));
        
        LOG_INFO("SUNAPI discovery probe sent to broadcast:7701");
        
        // Listen for responses (2 seconds)
        char buf[2048];
        sockaddr_in sender;
        socklen_t sender_len = sizeof(sender);
        auto start = std::chrono::steady_clock::now();
        
        while (std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - start).count() < 2) {
            if (session->stop_flag) break;
            
            int len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&sender, &sender_len);
            if (len > 0) {
                buf[len] = '\0';
                std::string response(buf, len);
                
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sender.sin_addr, ip_str, INET_ADDRSTRLEN);
                std::string ip(ip_str);
                
                if (isAlreadyDiscovered(session, ip)) continue;
                
                DeviceInfo info;
                info.ip = ip;
                info.port = 80;
                info.vendor = "hanwha";
                info.discovery_method = "sunapi";
                info.is_auth_success = false;
                info.rtsp_url = "rtsp://" + ip + ":554/profile1/media.smp";
                
                // Parse SUNAPI response for model/MAC if available
                // Response format: "Model=XNV-8080R\r\nMAC=00:09:18:xx:xx:xx\r\n..."
                std::regex model_re("Model=([^\\r\\n]+)");
                std::regex mac_re("MAC=([^\\r\\n]+)");
                std::regex fw_re("FirmwareVersion=([^\\r\\n]+)");
                std::smatch m;
                if (std::regex_search(response, m, model_re)) info.model = m[1].str();
                if (std::regex_search(response, m, mac_re)) info.mac_address = m[1].str();
                if (std::regex_search(response, m, fw_re)) info.firmware = m[1].str();
                
                addDevice(session, info);
                LOG_INFO("SUNAPI discovered Hanwha camera: {} (Model: {})", ip, info.model);
            } else {
                break; // Timeout or error
            }
        }

#ifdef _WIN32
        closesocket(sock);
        WSACleanup();
#else
        close(sock);
#endif
        LOG_INFO("SUNAPI Discovery complete.");
    } catch (const std::exception& e) {
        LOG_ERROR("SUNAPI Discovery error: {}", e.what());
    } catch (...) {
        LOG_ERROR("SUNAPI Discovery unknown error");
    }
}

// ═══════════════════════════════════════════════════════════════
//  Brute Force HTTP Auth
// ═══════════════════════════════════════════════════════════════
static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    return size * nmemb; // Ignore body
}

bool NetworkScanner::attemptBruteForce(const std::string& ip, int port, DeviceInfo& result) {
    std::vector<std::pair<std::string, std::string>> common_creds = {
        {"admin", "admin"},
        {"admin", "12345"},
        {"admin", "123456"},
        {"admin", "Hua12345"},
        {"root", "root"},
        {"root", "pass"},
        {"admin", ""}
    };

    if (port == 554) {
        result.vendor = "unknown_rtsp";
        result.is_auth_success = false;
        result.rtsp_url = "rtsp://" + ip + ":" + std::to_string(port) + "/live";
        return true;
    }

    CURL *curl;
    CURLcode res;
    
    bool found = false;
    std::string test_url = "http://" + ip + ":" + std::to_string(port) + "/";

    for (const auto& cred : common_creds) {
        curl = curl_easy_init();
        if(curl) {
            std::string userpwd = cred.first + ":" + cred.second;

            curl_easy_setopt(curl, CURLOPT_URL, test_url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC | CURLAUTH_DIGEST);
            curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1500L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);

            res = curl_easy_perform(curl);
            
            long response_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            
            curl_easy_cleanup(curl);

            if ((res == CURLE_OK) && (response_code != 401 && response_code != 403 && response_code != 0)) {
                result.username = cred.first;
                result.password = cred.second;
                result.vendor = "generic_cctv";
                result.is_auth_success = true;
                result.rtsp_url = "rtsp://" + userpwd + "@" + ip + ":554/Streaming/Channels/101";
                found = true;
                break;
            }
        }
    }
    
    if (!found) {
        result.is_auth_success = false;
        result.vendor = "unknown_http";
        result.rtsp_url = "rtsp://" + ip + ":554/live";
    }
    
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Phase 2: Multi-Threaded IP Range Scan
// ═══════════════════════════════════════════════════════════════
void NetworkScanner::scanIpBatch(std::shared_ptr<ScanSession> session,
                                  const std::string& base_ip,
                                  int start_ip, int end_ip,
                                  const std::vector<int>& ports) {
    for (int i = start_ip; i <= end_ip; i++) {
        if (session->stop_flag) return;
        
        std::string ip = base_ip + "." + std::to_string(i);
        
        // Skip if already discovered (by ONVIF/SUNAPI) or cached
        if (isAlreadyDiscovered(session, ip) || isCached(ip)) continue;
        
        for (int port : ports) {
            if (session->stop_flag) return;
            
            if (checkPort(ip, port)) {
                DeviceInfo info;
                info.ip = ip;
                info.port = port;
                info.discovery_method = "ip_scan";
                
                if (session->config.enable_brute_force) {
                    attemptBruteForce(ip, port, info);
                } else {
                    info.vendor = "unknown";
                    info.is_auth_success = false;
                    info.rtsp_url = "rtsp://" + ip + ":" + std::to_string(port) + "/stream";
                }
                
                addDevice(session, info);
                break; // Found open port, skip other ports for this IP
            }
        }
    }
}

void NetworkScanner::multiThreadScan(std::shared_ptr<ScanSession> session,
                                      const std::string& base_ip,
                                      const std::vector<int>& ports) {
    LOG_INFO("Phase 2: Multi-threaded IP scan starting...");
    session->phase = "ip_scan";
    
    // Thread count: auto based on CPU cores, min 4, max 32
    unsigned int hw_threads = std::thread::hardware_concurrency();
    unsigned int num_threads = std::max(4u, std::min(hw_threads > 0 ? hw_threads : 8, 32u));
    
    LOG_INFO("Using {} threads for IP scan (HW concurrency: {})", num_threads, hw_threads);
    
    // Split 1-254 into batches
    int total_ips = 254;
    int batch_size = (total_ips + num_threads - 1) / num_threads;
    
    std::vector<std::future<void>> futures;
    
    for (unsigned int t = 0; t < num_threads; t++) {
        int start = t * batch_size + 1;
        int end = std::min((int)(t + 1) * batch_size, total_ips);
        if (start > total_ips) break;
        
        futures.push_back(std::async(std::launch::async, 
            &NetworkScanner::scanIpBatch, this, session, base_ip, start, end, ports));
    }
    
    // Wait for all threads, updating progress periodically
    auto scan_start = std::chrono::steady_clock::now();
    size_t completed = 0;
    
    while (completed < futures.size()) {
        completed = 0;
        for (auto& f : futures) {
            if (f.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready) {
                completed++;
            }
        }
        
        // Update progress: 20% (ONVIF/SUNAPI done) + 80% * (completed/total)
        int ip_progress = (int)((float)completed / (float)futures.size() * 80.0f);
        session->progress = 20 + ip_progress;
        
        if (session->stop_flag) break;
    }
    
    // Ensure all futures complete
    for (auto& f : futures) {
        try { f.get(); } catch (...) {}
    }
    
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - scan_start).count();
    LOG_INFO("Multi-threaded IP scan complete in {}ms", elapsed);
}

// ═══════════════════════════════════════════════════════════════
//  Main Worker Loop (orchestrates all phases)
// ═══════════════════════════════════════════════════════════════
void NetworkScanner::workerLoop(std::shared_ptr<ScanSession> session) {
    auto total_start = std::chrono::steady_clock::now();
    
    session->status = "running";
    session->progress = 0;
    
    // ── Phase 0: Load cached devices ─────────────────────────
    {
        auto cached = getCachedDevices();
        if (!cached.empty()) {
            LOG_INFO("Loaded {} cached devices", cached.size());
            for (auto& dev : cached) {
                addDevice(session, dev);
            }
        }
    }
    session->progress = 5;

    // ── Phase 1: Parallel ONVIF + SUNAPI Discovery ───────────
    // Run both in parallel for maximum speed
    {
        std::future<void> onvif_future = std::async(std::launch::async, 
            &NetworkScanner::performWSDiscovery, this, session);
        std::future<void> sunapi_future = std::async(std::launch::async, 
            &NetworkScanner::performSunapiDiscovery, this, session);
        
        // Wait for both to complete (max ~4s each)
        try { onvif_future.get(); } catch (...) {}
        try { sunapi_future.get(); } catch (...) {}
    }
    session->progress = 20;
    
    if (session->stop_flag) {
        session->status = "stopped";
        return;
    }

    // ── Phase 2: Multi-threaded IP scan ──────────────────────
    std::string base_ip = session->config.network_range;
    if (base_ip.find("/") != std::string::npos) {
        base_ip = base_ip.substr(0, base_ip.find_last_of("."));
    }
    
    std::vector<int> ports = session->config.custom_ports;
    if (ports.empty()) {
        ports = {80, 443, 554, 8000, 37777, 8080}; // Common CCTV ports
    }

    multiThreadScan(session, base_ip, ports);
    
    // ── Done ─────────────────────────────────────────────────
    session->progress = 100;
    session->phase = "done";
    if (session->status != "stopped") {
        session->status = "completed";
    }
    
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - total_start).count();
    
    size_t dev_count;
    {
        std::lock_guard<std::mutex> dlock(session->devices_mutex);
        dev_count = session->devices.size();
    }
    
    LOG_INFO("═══ SCAN COMPLETE ═══ {} devices found in {}ms", dev_count, elapsed);
}

} // namespace core
} // namespace vms
