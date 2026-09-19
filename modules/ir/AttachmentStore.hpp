#pragma once
// =============================================================
// modules/ir/AttachmentStore.hpp — the one way a file becomes an ir.attachment.
//
// Two front doors store files: POST /web/attachment/upload (the browser, with
// a session cookie) and POST /api/v1/tickets/{key}/attachments (a script, with
// an API key). They must apply the SAME rules — the size cap, the type
// allowlist, the basename-only name, the document classification — or the
// weaker door becomes the way in. So the rules live here, once, and each door
// keeps only what is genuinely its own: how the caller is authenticated and how
// the multipart body is parsed.
// =============================================================
#include <pqxx/pqxx>
#include <stdexcept>
#include <string>

namespace cerp::modules::ir {

/// 25 MB. A datasheet is a few MB; this stops one request filling the disk.
constexpr long long kMaxUploadBytes = 25LL * 1024 * 1024;

/// A file refused for a reason the uploader can act on. `status` is the HTTP
/// code a route should answer with (400, or 413 for "too large").
class UploadRejected : public std::runtime_error {
public:
    UploadRejected(int status, const std::string& msg) : std::runtime_error(msg), status(status) {}
    int status;
};

struct StoredAttachment {
    int         id = 0;
    std::string name;
    std::string mimetype;
    long long   size = 0;
    std::string checksum;
    std::string documentType;
};

/// "screenshot.png" -> "image/png"; "" when the type is not allowed.
std::string uploadMimeFor(const std::string& fileName);

/// docs/106 — classify from the filename (gerber, drill, image, ...).
std::string classifyDocument(const std::string& lowerName);

/// The document_type vocabulary; anything else is not stored.
bool documentTypeAllowed(const std::string& type);

/**
 * Validate and store one file, inside the caller's transaction.
 *
 *   content   the bytes
 *   fileName  the name the client sent; only its basename is kept
 *   dispName  the display name ("" = the basename)
 *   resModel / resId   what it is attached to ("" / 0 = nothing yet);
 *             the CALLER checks that the record exists and may be written
 *   uid       stored as create_uid
 *   docType   explicit document_type, or "" to classify from the name
 *
 * Throws UploadRejected for an empty, oversized or disallowed file.
 */
StoredAttachment storeAttachment(pqxx::work& txn,
                                 const std::string& content,
                                 const std::string& fileName,
                                 const std::string& dispName,
                                 const std::string& description,
                                 const std::string& resModel,
                                 int resId,
                                 int uid,
                                 const std::string& docType);

} // namespace cerp::modules::ir
