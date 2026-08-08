// =============================================================
// core/Filestore.cpp
// =============================================================
#include "Filestore.hpp"

#include <openssl/evp.h>

#include <unistd.h>   // getpid

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace odoo::core {

std::string Filestore::root() {
    return "data/filestore";
}

std::string Filestore::sha256Hex(const std::string& bytes) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  mdLen = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("Filestore: EVP_MD_CTX_new failed");
    // RAII-free but exception-safe via a tiny guard.
    struct Guard { EVP_MD_CTX* c; ~Guard() { EVP_MD_CTX_free(c); } } guard{ctx};

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, bytes.data(), bytes.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, md, &mdLen) != 1) {
        throw std::runtime_error("Filestore: sha256 digest failed");
    }

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(mdLen * 2);
    for (unsigned int i = 0; i < mdLen; ++i) {
        out.push_back(hex[(md[i] >> 4) & 0xF]);
        out.push_back(hex[md[i] & 0xF]);
    }
    return out;
}

std::string Filestore::pathFor(const std::string& storeFname) {
    // store_fname is `<h[:2]>/<h>`, built by us from a hex hash, so it can
    // only contain [0-9a-f/]. Reject anything else defensively — the path
    // is never allowed to leave the root.
    if (storeFname.empty()) return "";
    for (char c : storeFname)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '/'))
            return "";
    if (storeFname.find("..") != std::string::npos) return "";
    return root() + "/" + storeFname;
}

StoredFile Filestore::put(const std::string& bytes) {
    const std::string h = sha256Hex(bytes);
    const std::string rel = h.substr(0, 2) + "/" + h;
    const std::string dir = root() + "/" + h.substr(0, 2);
    const std::string abs = dir + "/" + h;

    StoredFile sf;
    sf.checksum   = h;
    sf.storeFname = rel;
    sf.size       = static_cast<long long>(bytes.size());

    // Same content, same path: if it is already there, do not rewrite.
    std::error_code ec;
    if (fs::exists(abs, ec)) return sf;

    fs::create_directories(dir, ec);
    // Write to a temp name in the same directory, then rename — so a
    // reader never sees a half-written file, and two concurrent writers
    // of the same content cannot corrupt each other.
    const std::string tmp = abs + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("Filestore: cannot open " + tmp);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!f) throw std::runtime_error("Filestore: write failed for " + tmp);
    }
    fs::rename(tmp, abs, ec);
    if (ec) {
        // Lost a race, or the target now exists — either way the content
        // is correct because it is hash-addressed. Drop our temp.
        fs::remove(tmp, ec);
    }
    return sf;
}

std::string Filestore::get(const std::string& storeFname) {
    const std::string abs = pathFor(storeFname);
    if (abs.empty()) return "";
    std::ifstream f(abs, std::ios::binary);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

void Filestore::gc(const std::string& storeFname, long long remainingRefs) {
    // Only remove the shared blob when the LAST attachment referencing it
    // is gone. Deduplication means the file is not owned by one row.
    if (remainingRefs > 0) return;
    const std::string abs = pathFor(storeFname);
    if (abs.empty()) return;
    std::error_code ec;
    fs::remove(abs, ec);
}

} // namespace odoo::core
