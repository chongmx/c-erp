// =============================================================
// modules/ir/AttachmentStore.cpp — see AttachmentStore.hpp
// =============================================================
#include "AttachmentStore.hpp"
#include "Filestore.hpp"
#include <cctype>
#include <set>
#include <vector>

namespace cerp::modules::ir {

namespace {
std::string lowerBasename(const std::string& fileName) {
    std::string base = fileName;
    if (auto p = base.find_last_of("/\\"); p != std::string::npos) base = base.substr(p + 1);
    return base;
}
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool endsWith(const std::string& s, const std::string& e) {
    return s.size() > e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}
} // namespace

std::string uploadMimeFor(const std::string& fileName) {
    // Datasheets, the usual attachments, and manufacturing data (docs/106).
    // Deliberately no executable or script types — the allowlist is the
    // control, so it is extended by naming formats rather than by loosening
    // the rule.
    //
    // Every entry below is inert data: Gerber, Excellon drill, IPC
    // pick-and-place, STEP/DXF/STL geometry and EDA project files are read by
    // fabrication tools, never executed by the server or the browser. They are
    // served as application/octet-stream so a browser downloads them instead
    // of trying to render them.
    struct Ext { const char* e; const char* mime; };
    static const std::vector<Ext> kAllowed = {
        // documents and images
        {".pdf","application/pdf"}, {".png","image/png"},
        {".jpg","image/jpeg"}, {".jpeg","image/jpeg"},
        {".gif","image/gif"}, {".svg","image/svg+xml"},
        {".csv","text/csv"}, {".txt","text/plain"},
        {".xlsx","application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {".docx","application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {".zip","application/zip"},
        // Gerber — the fab data itself. Extended (.gbr/.ger) and the per-layer
        // conventions Altium and KiCad emit.
        {".gbr","application/octet-stream"}, {".ger","application/octet-stream"},
        {".gbl","application/octet-stream"}, {".gtl","application/octet-stream"},
        {".gbs","application/octet-stream"}, {".gts","application/octet-stream"},
        {".gbo","application/octet-stream"}, {".gto","application/octet-stream"},
        {".gm1","application/octet-stream"}, {".gko","application/octet-stream"},
        {".gbp","application/octet-stream"}, {".gtp","application/octet-stream"},
        {".gpt","application/octet-stream"}, {".gpb","application/octet-stream"},
        // Excellon drill / route
        {".drl","application/octet-stream"}, {".xln","application/octet-stream"},
        {".drd","application/octet-stream"}, {".tap","application/octet-stream"},
        // assembly / placement
        {".pos","text/plain"}, {".xy","text/plain"},
        // mechanical geometry
        {".step","application/octet-stream"}, {".stp","application/octet-stream"},
        {".iges","application/octet-stream"}, {".igs","application/octet-stream"},
        {".stl","application/octet-stream"}, {".dxf","application/octet-stream"},
        {".3mf","application/octet-stream"},
        // EDA project files
        {".kicad_pcb","application/octet-stream"},
        {".kicad_sch","application/octet-stream"},
        {".sch","application/octet-stream"}, {".brd","application/octet-stream"},
        {".net","text/plain"},
    };
    const std::string l = lower(lowerBasename(fileName));
    for (const auto& a : kAllowed) if (endsWith(l, a.e)) return a.mime;
    return {};
}

// docs/106 — classify an attachment from its filename.
//
// Auto-classification is the default because nobody labels sixteen Gerber
// layers by hand, and an unlabelled fabrication package is exactly the pile
// this feature exists to organise. An explicit document_type always overrides
// it, so the guess is a starting point rather than a verdict.
//
// Ambiguous extensions deliberately fall through to "document": a PDF may be a
// datasheet, an assembly drawing or a test report, and guessing between those
// is worse than leaving it for a person to say.
std::string classifyDocument(const std::string& lowerName) {
    struct Rule { const char* ext; const char* type; };
    static const Rule kRules[] = {
        // fabrication
        {".gbr","gerber"}, {".ger","gerber"}, {".gtl","gerber"}, {".gbl","gerber"},
        {".gto","gerber"}, {".gbo","gerber"}, {".gts","gerber"}, {".gbs","gerber"},
        {".gm1","gerber"}, {".gko","gerber"}, {".gbp","gerber"}, {".gtp","gerber"},
        {".gpt","gerber"}, {".gpb","gerber"},
        {".drl","drill"},  {".xln","drill"},  {".drd","drill"},  {".tap","drill"},
        {".pos","placement"}, {".xy","placement"},
        // design source
        {".kicad_pcb","pcb-design"}, {".brd","pcb-design"},
        {".kicad_sch","schematic"},  {".sch","schematic"},
        {".net","netlist"},
        // mechanical
        {".step","3d-model"}, {".stp","3d-model"}, {".iges","3d-model"},
        {".igs","3d-model"},  {".stl","3d-model"}, {".3mf","3d-model"},
        {".dxf","drawing"},
        // generic
        {".png","image"}, {".jpg","image"}, {".jpeg","image"},
        {".gif","image"}, {".svg","image"},
        {".csv","data"},  {".xlsx","data"},
        {".zip","archive"},
    };
    // .kicad_pcb must be tested before .pcb-like suffixes; the table order
    // does that, and the first match wins.
    for (const auto& r : kRules) if (endsWith(lowerName, r.ext)) return r.type;
    return "document";
}

/// The vocabulary the UI groups by. A value outside it is rejected rather than
/// stored, so a typo cannot quietly create a group of one.
bool documentTypeAllowed(const std::string& t) {
    static const std::set<std::string> k = {
        "gerber","drill","placement","pcb-design","schematic","netlist",
        "3d-model","drawing","datasheet","specification","image","data",
        "archive","document","other"
    };
    return k.count(t) > 0;
}

StoredAttachment storeAttachment(pqxx::work& txn,
                                 const std::string& content,
                                 const std::string& fileName,
                                 const std::string& dispName,
                                 const std::string& description,
                                 const std::string& resModel,
                                 int resId,
                                 int uid,
                                 const std::string& docType) {
    if (static_cast<long long>(content.size()) > kMaxUploadBytes)
        throw UploadRejected(413, "File exceeds the 25 MB limit");
    if (content.empty())
        throw UploadRejected(400, "Empty file");

    // Basename only, then the extension allowlist. SEC-16/SEC-19, the same
    // guard the portal proof upload uses. Storage is content-addressed, so the
    // name never reaches a path either way.
    const std::string base = lowerBasename(fileName);
    const std::string mime = uploadMimeFor(base);
    if (base.empty() || mime.empty())
        throw UploadRejected(400,
            "File type not allowed. Documents: pdf, png, jpg, gif, svg, csv, txt, "
            "xlsx, docx, zip. Manufacturing: gerber (gbr/ger/gtl/gbl/...), drill "
            "(drl/xln), placement (pos/xy), geometry (step/stp/stl/dxf/iges), "
            "EDA (kicad_pcb/kicad_sch/sch/brd/net).");

    std::string type = docType;
    if (type.empty() || !documentTypeAllowed(type)) type = classifyDocument(lower(base));

    const auto stored = core::Filestore::put(content);
    StoredAttachment out;
    out.name = dispName.empty() ? base : dispName;
    out.mimetype = mime;
    out.size = stored.size;
    out.checksum = stored.checksum;
    out.documentType = type;

    pqxx::params p;
    p.append(out.name); p.append(description);
    if (resModel.empty()) p.append(nullptr); else p.append(resModel);
    if (resId > 0) p.append(resId); else p.append(nullptr);
    p.append(mime);
    p.append(stored.size);
    p.append(stored.checksum);
    p.append(stored.storeFname);
    p.append(uid);
    p.append(type);
    auto ins = txn.exec(
        "INSERT INTO ir_attachment "
        "(name, description, res_model, res_id, type, mimetype, "
        " file_size, checksum, store_fname, create_uid, document_type) "
        "VALUES ($1,$2,$3,$4,'binary',$5,$6,$7,$8,$9,$10) RETURNING id", p);
    out.id = ins[0][0].as<int>();
    return out;
}

} // namespace cerp::modules::ir
