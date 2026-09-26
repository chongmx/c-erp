// ============================================================
// tests/unit/ai_reply/test_ai_reply.cpp — reading JSON out of a model's answer
//
// Reported: asking the part lookup for "8Mhz temperature controlled crystal"
// came back as "The AI agent do not reply with valid json text".
//
// The rule was "from the first { to the last }". That is fine for a reply
// wrapped in prose and wrong for a reply that was CUT OFF: the last } in the
// text belongs to some inner object, so the substring is a fragment, and both
// causes — a chatty model and a truncated one — produced the same message and
// the same dead end.
//
// These are pure functions on a string, so they are tested here rather than
// through somebody else's model over the network.
// ============================================================
#include "AiReply.hpp"
#include "TestHarness.hpp"

#include <string>

using cerp::modules::ir::extractJsonObject;
using cerp::modules::ir::extractJsonObjects;
using cerp::modules::ir::pickAnswerObject;
using cerp::modules::ir::looksTruncated;
using erptest::section;

static void ck(bool cond, const std::string& what) { erptest::check(cond, what); }

static void eqs(const std::string& got, const std::string& want, const std::string& what) {
    erptest::check(got == want,
        got == want ? what
                    : what + "\n            got  '" + got + "'\n            want '" + want + "'");
}

ERP_TEST(AiReply, extract) {
    // ---------------------------------------------------------
    section("the ordinary cases");
    {
        auto j = extractJsonObject(R"({"notes":"ok","candidates":[]})");
        ck(!j.is_null(), "a bare object parses");
        eqs(j.value("notes", ""), "ok", "and its fields are readable");
    }
    {
        // What every model does at least sometimes, however firmly the prompt
        // says not to.
        auto j = extractJsonObject("Here is what I found:\n```json\n"
                                   R"({"notes":"fenced","candidates":[]})"
                                   "\n```\nHope that helps!");
        ck(!j.is_null(), "prose and code fences around it are ignored");
        eqs(j.value("notes", ""), "fenced", "the object inside is what is read");
    }
    {
        auto j = extractJsonObject(R"({"a":{"b":{"c":1}},"d":2})");
        ck(!j.is_null() && j["d"] == 2, "nested objects close at the right brace");
    }

    // ---------------------------------------------------------
    section("braces inside strings");
    // The old first-{-to-last-} rule survived these by accident; brace
    // counting only survives them by being string-aware.
    {
        auto j = extractJsonObject(R"({"notes":"a } inside a string","x":1})");
        ck(!j.is_null() && j["x"] == 1, "a closing brace inside a string is not the end");
    }
    {
        auto j = extractJsonObject(R"({"notes":"an escaped \" quote and a }","x":2})");
        ck(!j.is_null() && j["x"] == 2, "an escaped quote does not end the string");
    }
    {
        // A trailing sentence containing braces is exactly what breaks
        // first-{-to-last-}: it would swallow the prose and fail to parse.
        auto j = extractJsonObject(R"({"ok":true} — note the {curly} braces above.)");
        ck(!j.is_null() && j["ok"] == true, "text after the object, with braces in it, is ignored");
    }
    {
        // Two objects: the first is the answer, the second is commentary.
        auto j = extractJsonObject(R"({"first":1} and then {"second":2})");
        ck(!j.is_null() && j.contains("first"), "the FIRST complete object wins");
    }

    // ---------------------------------------------------------
    section("truncation — the case that was reported");
    {
        // A reasoning model spends its output budget thinking and the JSON
        // stops mid-string. This is what 8 MHz TCXO produced.
        const std::string cut =
            R"({"notes":"Searching for an 8 MHz TCXO","candidates":[{"mpn":"ECS-TXO-8)";
        ck(extractJsonObject(cut).is_null(), "a cut-off reply yields no object");
        ck(looksTruncated(cut), "and is recognised as truncated, not as gibberish");
    }
    {
        const std::string cut = R"({"candidates":[{"mpn":"X"},{"mpn":"Y"}],)";
        ck(extractJsonObject(cut).is_null(), "stopping after a complete inner object is still truncated");
        ck(looksTruncated(cut), "the outer object never closed");
    }
    {
        // The distinction that matters: this is not truncated, it is wrong.
        // Telling someone to raise the token ceiling would waste their time.
        const std::string junk = "I could not find that part.";
        ck(extractJsonObject(junk).is_null(), "prose with no object yields nothing");
        ck(!looksTruncated(junk), "and is NOT reported as truncated");
    }
    {
        const std::string complete = R"({"notes":"done","candidates":[]}   )";
        ck(!looksTruncated(complete), "a complete object is never truncated");
    }
    {
        // An open string at the end is the commonest shape of a cut-off reply.
        ck(looksTruncated(R"({"notes":"half a sen)"), "an unterminated string counts as truncated");
    }

    // ---------------------------------------------------------
    section("a narrating model — the reply that was actually reported");
    {
        // Recorded from production, job 1, "8Mhz temperature controlled
        // crystal": grok wrote an interim object with NOTHING in it, a
        // sentence of prose, and then the real answer. First-{-to-last-}
        // spanned the lot and parsed nothing; taking the first complete
        // object gets the empty one and calls a good reply empty.
        const std::string real =
            R"({"notes":"Searching manufacturer and distributor listings for an 8 MHz )"
            R"(temperature-controlled crystal (TCXO). The request is a description, not an )"
            R"(MPN, so several package, stability, and supply options exist.","candidates":[]})"
            "The description is ambiguous (TCXO vs OCXO, no MPN). Searching distributor "
            "listings for stocked 8 MHz parts."
            R"({"notes":"Description only.","candidates":[{"mpn":"ECS-TXO-8","name":"8 MHz TCXO"}]})";

        auto objs = extractJsonObjects(real);
        ck(objs.size() == 2, "both objects are found, prose between them ignored");

        auto answer = pickAnswerObject(objs);
        ck(!answer.is_null(), "an answer is chosen");
        ck(answer["candidates"].is_array() && answer["candidates"].size() == 1,
           "and it is the one with candidates in it, not the empty narration");
        eqs(answer["candidates"][0].value("mpn", ""), "ECS-TXO-8",
            "so the part that was found is the part that comes back");
    }
    {
        // An honest empty answer is still an answer: one object, nothing in
        // it, and nothing else in the reply to prefer over it.
        auto objs = extractJsonObjects(R"({"notes":"nothing found","candidates":[]})");
        auto answer = pickAnswerObject(objs);
        ck(!answer.is_null() && answer["candidates"].empty(),
           "a single empty result is passed through as itself");
    }
    {
        // Narration first, answer last — and the answer is the LAST one even
        // when several carry candidates.
        auto objs = extractJsonObjects(
            R"({"candidates":[{"mpn":"FIRST"}]} then {"candidates":[{"mpn":"LAST"}]})");
        auto answer = pickAnswerObject(objs);
        eqs(answer["candidates"][0].value("mpn", ""), "LAST",
            "the last object with findings wins — a model narrates forwards");
    }
    {
        // The trailing object is cut off, so only the complete one is usable.
        auto objs = extractJsonObjects(
            R"({"candidates":[{"mpn":"COMPLETE"}]} more text {"candidates":[{"mpn":"CU)");
        ck(objs.size() == 1, "a truncated trailing object is not half-read");
        eqs(pickAnswerObject(objs)["candidates"][0].value("mpn", ""), "COMPLETE",
            "and the complete one before it is still the answer");
    }

    // ---------------------------------------------------------
    section("nothing to read");
    ck(extractJsonObject("").is_null(),      "empty text yields nothing");
    ck(!looksTruncated(""),                  "and empty is not truncated");
    ck(extractJsonObject("[]").is_null(),    "a bare array is not the object shape asked for");
    ck(extractJsonObject("{not json}").is_null(), "a malformed object yields nothing");
    ck(!looksTruncated("{not json}"),        "and is malformed, not truncated");
}
