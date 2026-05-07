// email_sender.cpp — see email_sender.h header for design notes.

#include "utils/email_sender.h"

#include "core/runtime_state.h"
#include "database/db_manager.h"
#include "utils/background_job_runner.h"
#include "utils/logger.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <sstream>

namespace vms::utils {

namespace {

constexpr std::size_t kMaxBodyBytes = 8192;

vms::utils::BackgroundJobRunner& runner() {
    static vms::utils::BackgroundJobRunner instance("email-sender", 2, 128);
    return instance;
}

std::string rfc2822Date() {
    std::time_t t = std::time(nullptr);
    std::tm gm{};
#if defined(_WIN32)
    gmtime_s(&gm, &t);
#else
    gmtime_r(&t, &gm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S +0000", &gm);
    return buf;
}

} // namespace

std::string sanitizeMailHeader(const std::string& s, std::size_t max_len) {
    std::string out;
    out.reserve(std::min(s.size(), max_len));
    for (char c : s) {
        if (c == '\r' || c == '\n') continue;
        out.push_back(c);
        if (out.size() >= max_len) break;
    }
    return out;
}

bool sendEmailAsync(const std::string& subject,
                    const std::string& body,
                    const std::vector<std::string>& recipients,
                    const std::string& source_label) {
    if (recipients.empty()) return false;

    auto& db = vms::database::DbManager::getInstance();
    const std::string smtp_host = db.getSetting("smtp_host", "");
    const std::string smtp_from = db.getSetting("smtp_from", "");
    if (smtp_host.empty() || smtp_from.empty()) {
        LOG_WARN("[email_sender:{}] SMTP not configured (smtp_host/smtp_from). {} email(s) skipped.",
                 source_label, recipients.size());
        return false;
    }

    const std::string smtp_port = db.getSetting("smtp_port", "587");
    const std::string smtp_user = db.getSetting("smtp_user", "");
    const std::string smtp_pass = db.getSetting("smtp_pass", "");
    const bool use_tls = db.getSetting("smtp_tls", "true") != "false";

    const std::string subject_clean = sanitizeMailHeader(subject);
    const std::string from_clean = sanitizeMailHeader(smtp_from);
    const std::string date = rfc2822Date();

    std::string body_capped = body;
    if (body_capped.size() > kMaxBodyBytes) body_capped.resize(kMaxBodyBytes);

    bool any_queued = false;
    for (const auto& raw_to : recipients) {
        const std::string to_clean = sanitizeMailHeader(raw_to, 320); // RFC 5321
        if (to_clean.empty() || to_clean.find('@') == std::string::npos) {
            LOG_WARN("[email_sender:{}] skip invalid recipient: '{}'", source_label, raw_to);
            continue;
        }

        std::ostringstream msg;
        msg << "Date: " << date << "\r\n"
            << "From: " << from_clean << "\r\n"
            << "To: " << to_clean << "\r\n"
            << "Subject: " << subject_clean << "\r\n"
            << "MIME-Version: 1.0\r\n"
            << "Content-Type: text/plain; charset=UTF-8\r\n"
            << "Content-Transfer-Encoding: 8bit\r\n"
            << "\r\n"
            << body_capped;

        std::string payload = msg.str();
        std::string url = (use_tls ? "smtps://" : "smtp://") + smtp_host + ":" + smtp_port;

        const std::string label = source_label;
        if (!runner().submit(
                [url, smtp_user, smtp_pass, from_clean, to_clean, payload, use_tls, label]() {
            if (vms::core::shutting_down.load(std::memory_order_acquire)) return;
            CURL* curl = curl_easy_init();
            if (!curl) return;

            struct UploadCtx { const char* p; size_t left; };
            UploadCtx ctx{ payload.c_str(), payload.size() };
            auto reader = +[](char* buf, size_t size, size_t nmemb, void* userp) -> size_t {
                auto* c = static_cast<UploadCtx*>(userp);
                size_t want = size * nmemb;
                size_t n = std::min(want, c->left);
                if (n == 0) return 0;
                std::memcpy(buf, c->p, n);
                c->p += n;
                c->left -= n;
                return n;
            };

            struct curl_slist* rcpt = nullptr;
            rcpt = curl_slist_append(rcpt, to_clean.c_str());

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            if (!smtp_user.empty()) {
                curl_easy_setopt(curl, CURLOPT_USERNAME, smtp_user.c_str());
                curl_easy_setopt(curl, CURLOPT_PASSWORD, smtp_pass.c_str());
            }
            if (use_tls && url.rfind("smtp://", 0) == 0) {
                curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
            }
            curl_easy_setopt(curl, CURLOPT_MAIL_FROM, from_clean.c_str());
            curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, rcpt);
            curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(curl, CURLOPT_READFUNCTION, reader);
            curl_easy_setopt(curl, CURLOPT_READDATA, &ctx);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                LOG_ERROR("[email_sender:{}] SMTP send to {} failed: {}",
                          label, to_clean, curl_easy_strerror(res));
            } else {
                LOG_INFO("[email_sender:{}] SMTP delivered to {}", label, to_clean);
            }
            curl_slist_free_all(rcpt);
            curl_easy_cleanup(curl);
        })) {
            LOG_THROTTLED_WARN(5000, "[email_sender:{}] queue full, dropping email send", source_label);
            continue;
        }
        any_queued = true;
    }
    return any_queued;
}

} // namespace vms::utils
