#pragma once
// =============================================================
// modules/mail/MailHelpers.hpp
//
// Shared inline helper for writing audit-log entries to
// mail_message.  Include this header in any ViewModel that
// needs to call postLog() inside an open pqxx::work.
//
// Prerequisite: MailModule must be registered before any
// module that calls postLog() so that the mail_message
// table is created by ensureSchema_() at startup.
// =============================================================
#include <pqxx/pqxx>
#include <string>

namespace odoo::modules::mail {

// Insert a single audit-log row into mail_message.
// authorId == 0  → stored as NULL (displayed as "System").
// subtype        → "note" (default) | "log_note" | "comment"
inline void postLog(
    pqxx::work&        txn,
    const std::string& resModel,
    int                resId,
    int                authorId,
    const std::string& body,
    const std::string& subtype = "note")
{
    txn.exec(
        "INSERT INTO mail_message "
        "(res_model, res_id, author_id, body, subtype, date) "
        "VALUES ($1, $2, NULLIF($3, 0), $4, $5, now())",
        pqxx::params{resModel, resId, authorId, body, subtype});
}

} // namespace odoo::modules::mail
