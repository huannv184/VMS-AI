// email_sender.h — shared SMTP send path used by the alert delivery layer
// (vms::events::deliverAction → EMAIL channel). Pre-2026-05-14 this helper
// was shared between vms::events::AlertRouter and vms::core::AlertManager;
// both have been retired in the AlertManager consolidation, leaving the
// unified RuleEngine-driven dispatch as the only caller.
//
//   1. Closes BUG-ALERT-02 (legacy email rules silently logged instead of
//      sending) — the only path now is async, real SMTP.
//   2. Removes a synchronous CURL call from EventManager's broadcast loop —
//      the helper queues every send through a dedicated background runner
//      with a bounded queue (drop-on-full).
//   3. Gives one place to maintain SMTP settings keys, header sanitisation,
//      and STARTTLS handling.
//
// Settings keys read from the `settings` table on every send (uncached so
// admin updates take effect without a backend restart):
//   smtp_host (required), smtp_from (required)
//   smtp_port (default "587"), smtp_user, smtp_pass
//   smtp_tls  (default "true" — set to "false" to disable STARTTLS for plain
//             smtp:// URLs; smtps:// URLs always use TLS)
//
// If smtp_host or smtp_from is empty, the helper logs a one-line WARN and
// returns false WITHOUT queueing anything. This is the "feature disabled"
// shape, NOT an error — operators see "configure SMTP first" rather than a
// queue full of doomed sends.
#pragma once

#include <string>
#include <vector>

namespace vms::utils {

// Strip CR/LF and bound length on a header field (RFC 5322 / 5321 hardening).
// Exposed so callers can pre-sanitise rule names / subjects before passing
// them to sendEmailAsync — the helper also calls this internally on
// recipient addresses, From and Subject.
std::string sanitizeMailHeader(const std::string& s, std::size_t max_len = 256);

// Queue one async send per recipient. Returns true if at least one send was
// queued; false if SMTP is not configured or every recipient was rejected
// (invalid address). Each queued job runs on a shared internal background
// runner (2 workers, queue size 128, drop-on-full).
//
// `subject` and `body` are sent as plain UTF-8 (Content-Type: text/plain).
// The body is capped at 8 KB at queue time to keep one runaway event from
// flooding the queue.
//
// `source_label` appears in log lines as `[email_sender:<label>]` so we can
// distinguish callers in production logs (today there is only `alert_delivery`,
// the post-consolidation unified path).
bool sendEmailAsync(const std::string& subject,
                    const std::string& body,
                    const std::vector<std::string>& recipients,
                    const std::string& source_label);

} // namespace vms::utils
