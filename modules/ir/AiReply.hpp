#pragma once
// ============================================================
// modules/ir/AiReply.hpp — reading JSON out of a model's answer.
//
// Pure functions, in their own header so they can be unit-tested. What lives
// here failed in production and could not be tested where it was: it was a
// private static inside a ViewModel, reachable only through a network call to
// somebody else's model.
//
// The job is narrow and the failures are specific:
//
//   * a model wraps its JSON in prose or code fences however firmly it is
//     told not to
//   * a model that reasons before it answers spends its output budget doing
//     so, and the JSON arrives CUT OFF — which is what "8Mhz temperature
//     controlled crystal" hit: a reply that starts well and stops mid-string
//
// The old rule was "from the first { to the last }", and a truncated reply
// defeats it: the last } in the text is some inner object's, so the substring
// is a fragment that cannot parse, and every cause came out as the same
// unhelpful "The reply was not valid JSON".
//
// Nothing here REPAIRS a truncated answer. A half-written candidate is a part
// number with a missing digit or a resistance without its unit, and guessing
// the rest is how a wrong part reaches a catalogue somebody orders from. The
// answer to truncation is to say so, and to say what to change.
// ============================================================
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace cerp::modules::ir {

/**
 * @brief The first balanced {...} in a reply, parsed.
 *
 * Brace counting is STRING-AWARE: a `}` inside "notes" is not the end of the
 * object, and `\"` inside that string does not end it either. Counting bare
 * braces gets both wrong, and gets them wrong on exactly the replies that
 * contain prose — which is all of them.
 *
 * @returns the parsed object, or null when there is no complete one
 */
inline nlohmann::json extractJsonObject(const std::string& text) {
    const auto start = text.find('{');
    if (start == std::string::npos) return nlohmann::json(nullptr);

    int depth = 0;
    bool inString = false, escaped = false;
    for (std::size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (escaped)            escaped = false;
            else if (c == '\\')     escaped = true;
            else if (c == '"')      inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) {
                auto j = nlohmann::json::parse(text.substr(start, i - start + 1),
                                               nullptr, false);
                return j.is_discarded() ? nlohmann::json(nullptr) : j;
            }
        }
    }
    // Ran out of text with the object still open: truncated, not malformed.
    return nlohmann::json(nullptr);
}

/**
 * @brief Every complete top-level {...} in the reply, in the order written.
 *
 * A searching model narrates. This is a real reply from grok, to "8Mhz
 * temperature controlled crystal":
 *
 *     {"notes":"Searching manufacturer and distributor listings…",
 *      "candidates":[]}The description is ambiguous (TCXO vs OCXO, no MPN).
 *     Searching distributor listings for stocked 8 MHz parts.{"notes":…
 *
 * — an interim object with nothing in it, a sentence of prose, then the
 * answer. First-{-to-last-} spans the lot and parses nothing; taking the
 * FIRST complete object gets the empty one and reports "no candidates" about
 * a reply that had them. Both are the same mistake: assuming there is one
 * object in there.
 *
 * @param maxObjects stop after this many; a reply is not a document store
 */
inline std::vector<nlohmann::json> extractJsonObjects(const std::string& text,
                                                      std::size_t maxObjects = 8) {
    std::vector<nlohmann::json> out;
    std::size_t pos = 0;
    while (out.size() < maxObjects) {
        const auto start = text.find('{', pos);
        if (start == std::string::npos) break;

        int depth = 0;
        bool inString = false, escaped = false, closed = false;
        std::size_t i = start;
        for (; i < text.size(); ++i) {
            const char c = text[i];
            if (inString) {
                if (escaped)            escaped = false;
                else if (c == '\\')     escaped = true;
                else if (c == '"')      inString = false;
                continue;
            }
            if (c == '"') { inString = true; continue; }
            if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) { closed = true; break; }
        }
        if (!closed) break;                       // the rest is truncated

        auto j = nlohmann::json::parse(text.substr(start, i - start + 1), nullptr, false);
        if (!j.is_discarded()) out.push_back(j);
        pos = i + 1;
    }
    return out;
}

/**
 * @brief Which of those objects is the ANSWER.
 *
 * The last one carrying a non-empty `arrayKey` wins: a model that narrates
 * writes its empty interim objects first and its findings last. Failing that,
 * the last object that mentions the key at all — an honest empty answer is
 * still an answer. Failing that, the last object, and if there are none, null.
 */
inline nlohmann::json pickAnswerObject(const std::vector<nlohmann::json>& objs,
                                       const char* arrayKey = "candidates") {
    const nlohmann::json* withItems = nullptr;
    const nlohmann::json* withKey   = nullptr;
    for (const auto& o : objs) {
        if (!o.is_object()) continue;
        auto it = o.find(arrayKey);
        if (it != o.end()) {
            withKey = &o;
            if (it->is_array() && !it->empty()) withItems = &o;
        }
    }
    if (withItems) return *withItems;
    if (withKey)   return *withKey;
    return objs.empty() ? nlohmann::json(nullptr) : objs.back();
}

/**
 * @brief Does this reply look CUT OFF rather than merely unparseable?
 *
 * Used when the provider does not say so itself. An object that opens and
 * never closes — or a string still open at the end — is a reply that stopped
 * mid-sentence, which means the model ran out of room rather than answered
 * badly. The two need different advice: raise the ceiling, versus look at
 * what it actually said.
 */
inline bool looksTruncated(const std::string& text) {
    const auto start = text.find('{');
    if (start == std::string::npos) return false;

    int depth = 0;
    bool inString = false, escaped = false;
    for (std::size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (escaped)        escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"')  inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return false;   // it closed: complete
    }
    return depth > 0 || inString;
}

} // namespace cerp::modules::ir
