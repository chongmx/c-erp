#pragma once
// =============================================================
// modules/website/WebsiteMedia.hpp — images for the public site (docs/124)
//
// The editor could place an image block, but only by typing a URL to a file
// hosted somewhere else: `/web/content/{id}` requires a session and ignores
// `ir_attachment.public` entirely, so an uploaded file could never be shown to
// a visitor. This is the missing half.
//
// Everything here exists because an uploaded file is attacker-controlled bytes
// that will later be served from OUR origin. Two rules follow:
//
//   * The type is SNIFFED, never believed. A browser's `Content-Type`, a file
//     extension and a filename are all supplied by whoever is uploading. Only
//     the leading bytes say what a file actually is, and only a file whose
//     bytes match a known image signature is stored at all.
//
//   * SVG IS REFUSED. It is the one "image" format that is really a document:
//     it is XML, it can carry <script>, and served from our own origin it
//     would run with our origin's privileges — a stored XSS with an <img>
//     tag for a delivery mechanism. There is no sanitiser here good enough to
//     make that safe, so the format is simply not accepted.
// =============================================================
#include <string>

namespace cerp::modules::website {

class WebsiteMedia {
public:
    /**
     * The mime type implied by the file's own leading bytes, or "" if the
     * bytes are not one of the accepted raster image formats.
     *
     * Accepts: PNG, JPEG, GIF, WebP. Everything else — including SVG, HTML,
     * PDF, ZIP and anything unrecognised — returns "".
     */
    static std::string sniff(const std::string& bytes);

    /// The file extension for an accepted mime type, without the dot.
    static std::string extensionFor(const std::string& mime);

    /**
     * A filename safe to put in a Content-Disposition header and to show in a
     * listing: basename only, charset-restricted, length-capped, and always
     * ending in the extension implied by `mime` rather than the one the
     * uploader claimed. Never empty.
     */
    static std::string safeName(const std::string& proposed, const std::string& mime);

    /// Is this one of the video types?
    static bool isVideo(const std::string& mime);

    /// The size ceiling for a given type — video is allowed more.
    static long long maxBytesFor(const std::string& mime);

    /// The largest image the site will store, in bytes.
    static constexpr long long kMaxBytes = 8LL * 1024 * 1024;
    /// …and the largest video. Longer clips belong on a provider; that is
    /// what the embed block is for.
    static constexpr long long kMaxVideoBytes = 24LL * 1024 * 1024;
};

} // namespace cerp::modules::website
