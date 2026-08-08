#pragma once
// =============================================================
// core/Filestore.hpp — content-addressed file storage for ir.attachment
//
// Bytes live on disk, addressed by SHA-256 of their content:
//
//     data/filestore/<h[:2]>/<h>
//
// The hash IS the name, so identical content stored twice occupies one
// file (a datasheet shared by many parts costs the disk once), and a
// filename from the request never touches the path — no traversal is
// possible because the path is derived, not supplied.
//
// This mirrors Odoo's filestore layout and the split the existing
// payment_proof table already uses: metadata in the DB, bytes on disk,
// chosen over a bytea column so a 20 MB datasheet does not enter every
// row SELECT and every pg_dump.
// =============================================================
#include <string>

namespace odoo::core {

struct StoredFile {
    std::string checksum;    ///< sha256 hex of the content
    std::string storeFname;  ///< filestore-relative path, `<h[:2]>/<h>`
    long long   size = 0;    ///< bytes
};

class Filestore {
public:
    /// Filesystem root of the store. Created on first write.
    static std::string root();

    /**
     * Write content, addressed by its hash. Idempotent: if the target
     * already exists (same content), it is not rewritten.
     */
    static StoredFile put(const std::string& bytes);

    /// Absolute path for a store_fname, or "" if it escapes the root.
    static std::string pathFor(const std::string& storeFname);

    /// Read the bytes back, or "" if absent.
    static std::string get(const std::string& storeFname);

    /**
     * Remove the file for a store_fname IF no other attachment row still
     * references it. The caller passes the reference count it observed in
     * the same transaction; 0 means safe to delete.
     */
    static void gc(const std::string& storeFname, long long remainingRefs);

    /// SHA-256 hex of a byte string.
    static std::string sha256Hex(const std::string& bytes);
};

} // namespace odoo::core
