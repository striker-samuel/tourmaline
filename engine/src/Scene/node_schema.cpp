// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// PROJECT-DEFINED NODE SCHEMAS - the implementation (see
// node_schema.h for the design record).

#include <Engine/Scene/node_schema.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace smvk {

namespace {

void AppendU32(std::vector<uint8_t>& out, uint32_t v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 4);
}

uint32_t AlignUp(uint32_t v, uint32_t align) {
    return (v + align - 1) & ~(align - 1);
}

// A single literal of the field's element type, appended in cooked
// form. Returns false on a malformed literal.
bool AppendLiteral(const std::string& tok, SchemaFieldType type, std::vector<uint8_t>& out) {
    const auto putF32 = [&](float f) {
        uint32_t bits = 0;
        std::memcpy(&bits, &f, 4);
        AppendU32(out, bits);
    };
    errno = 0;
    char* end = nullptr;
    switch (type) {
    // The scalar float + the VECTOR components (the caller splits a
    // vecN literal into perElem single-float literals — each call
    // appends exactly one f32).
    case SchemaFieldType::F32:
    case SchemaFieldType::Vec2:
    case SchemaFieldType::Vec3:
    case SchemaFieldType::Vec4: {
        const float v = std::strtof(tok.c_str(), &end);
        if (end == tok.c_str() || *end != '\0')
            return false;
        putF32(v);
        return true;
    }
    case SchemaFieldType::I32: {
        const long v = std::strtol(tok.c_str(), &end, 10);
        if (end == tok.c_str() || *end != '\0')
            return false;
        AppendU32(out, static_cast<uint32_t>(static_cast<int32_t>(v)));
        return true;
    }
    case SchemaFieldType::U32:
    case SchemaFieldType::Bool: {
        const unsigned long v =
            std::strtoul(tok.c_str(), &end, type == SchemaFieldType::Bool ? 10 : 0);
        if (end == tok.c_str() || *end != '\0')
            return false;
        if (type == SchemaFieldType::Bool && v > 1)
            return false;
        AppendU32(out, static_cast<uint32_t>(v));
        return true;
    }
    case SchemaFieldType::Ref: {
        // A stable content id: decimal or 0x-prefixed.
        const unsigned long long v = std::strtoull(tok.c_str(), &end, 0);
        if (end == tok.c_str() || *end != '\0')
            return false;
        const uint64_t id = v;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&id);
        out.insert(out.end(), p, p + 8);
        return true;
    }
    default:
        return false; // vec/str literals handled by the caller
    }
}

} // namespace

uint32_t SchemaTypeStride(SchemaFieldType type) {
    switch (type) {
    case SchemaFieldType::F32:
    case SchemaFieldType::I32:
    case SchemaFieldType::U32:
    case SchemaFieldType::Bool:
        return 4;
    case SchemaFieldType::Vec2:
        return 8;
    case SchemaFieldType::Vec3:
        return 12;
    case SchemaFieldType::Vec4:
        return 16;
    case SchemaFieldType::Str:
        return 4; // the length prefix (VARIABLE body; arrays of str
                  // are unsupported - count must be 1)
    case SchemaFieldType::Ref:
        return 8;
    }
    return 0;
}

const SchemaField* NodeSchema::Field(uint32_t fieldId) const {
    for (const SchemaField& f : fields)
        if (f.id == fieldId)
            return &f;
    return nullptr;
}

bool FinalizeSchema(NodeSchema& schema, std::string& err) {
    if (schema.name.empty() || schema.id == 0) {
        err = "the schema needs a name and a nonzero stable id";
        return false;
    }
    // The names become C++ IDENTIFIERS in the generated project::types
    // (and keys in the authored tooling): validate the shape HERE so
    // the generator can trust it ([A-Za-z_][A-Za-z0-9_]*).
    const auto isIdentifier = [](const std::string& s) {
        if (s.empty())
            return false;
        for (size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
                            (i > 0 && c >= '0' && c <= '9');
            if (!ok)
                return false;
        }
        return true;
    };
    if (!isIdentifier(schema.name)) {
        err = "the schema name '" + schema.name +
              "' is not a valid identifier (it becomes the generated"
              " project::types struct name)";
        return false;
    }
    uint32_t off = 0;
    bool sawStr = false;
    for (uint32_t i = 0; i < schema.fields.size(); ++i) {
        SchemaField& f = schema.fields[i];
        f.id = i; // the declaration ordinal IS the stable field id
        if (f.name.empty()) {
            err = "schema '" + schema.name + "' field #" + std::to_string(i) + " has no name";
            return false;
        }
        if (!isIdentifier(f.name)) {
            err = "schema '" + schema.name + "' field '" + f.name +
                  "' is not a valid identifier (it becomes the"
                  " generated member name)";
            return false;
        }
        if (f.count == 0) {
            err = "schema '" + schema.name + "' field '" + f.name + "' has count 0";
            return false;
        }
        f.stride = SchemaTypeStride(f.type);
        if (sawStr && f.type != SchemaFieldType::Str) {
            // str is layout-terminal: NOTHING may follow it (the
            // variable tail - the fixed prefix is what the generated
            // POD mirrors).
            err = "schema '" + schema.name + "' field '" + f.name +
                  "': str fields must be LAST (a variable-length"
                  " tail cannot be followed by fixed fields)";
            return false;
        }
        if (f.type == SchemaFieldType::Str) {
            if (f.count != 1) {
                err = "schema '" + schema.name + "' field '" + f.name +
                      "': str fields cannot be arrays";
                return false;
            }
            sawStr = true;
        }
        // ALIGNMENT = the type's NATURAL alignment, NOT its stride:
        // the generated C++ POD (float[3] for vec3, align 4) must lay
        // out identically to the cooked block - aligning vec3 to its
        // 12-byte stride would diverge from the C++ layout.
        const uint32_t align = f.type == SchemaFieldType::Ref ? 8u : 4u;
        off = AlignUp(off, align);
        f.offset = off;
        if (f.type == SchemaFieldType::Str) {
            // The variable tail: the default's cooked form (u32
            // length + body) padded to 4. The schema's valueBytes
            // carries the DEFAULT's size; a row with a different
            // string sizes its own block.
            off += f.defaultValue.size();
            while (off % 4)
                ++off;
        } else {
            off += f.stride * f.count;
        }
    }
    schema.valueBytes = off;
    // Duplicate field names refuse (the generated accessors key by
    // name; a dup would be ambiguous).
    for (uint32_t i = 0; i < schema.fields.size(); ++i)
        for (uint32_t j = i + 1; j < schema.fields.size(); ++j)
            if (schema.fields[i].name == schema.fields[j].name) {
                err = "schema '" + schema.name +
                      "' has duplicate"
                      " field '" +
                      schema.fields[i].name + "'";
                return false;
            }
    return true;
}

// ---- the authored text format ---------------------------------------------

bool ParseNodeSchemaText(const std::string& text, NodeSchema& out, std::string& err) {
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    bool sawHeader = false;
    while (std::getline(in, line)) {
        ++lineNo;
        // Strip comments (# to end) + trim.
        const size_t hash = line.find('#');
        if (hash != std::string::npos)
            line = line.substr(0, hash);
        size_t b = 0;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t' || line[b] == '\r'))
            ++b;
        size_t e = line.size();
        while (e > b && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r'))
            --e;
        if (b == e)
            continue;
        const std::string trimmed = line.substr(b, e - b);

        if (!sawHeader) {
            std::istringstream hs(trimmed);
            std::string kw, name, idTok;
            hs >> kw >> name >> idTok;
            if (kw != "schema" || name.empty() || idTok.empty()) {
                err = "line " + std::to_string(lineNo) + ": expected 'schema <name> <id>'";
                return false;
            }
            char* end = nullptr;
            errno = 0;
            const unsigned long id = std::strtoul(idTok.c_str(), &end, 0);
            if (end == idTok.c_str() || *end != '\0' || id == 0) {
                err = "line " + std::to_string(lineNo) + ": bad schema id '" + idTok + "'";
                return false;
            }
            out = NodeSchema{};
            out.name = name;
            out.id = static_cast<uint32_t>(id);
            sawHeader = true;
            continue;
        }
        // An attr line: attr <name> <type> [count] [= literals]
        std::istringstream as(trimmed);
        std::string kw, name, typeTok;
        as >> kw >> name >> typeTok;
        if (kw != "attr" || name.empty() || typeTok.empty()) {
            err = "line " + std::to_string(lineNo) +
                  ": expected 'attr <name> <type> [count] [= literals]'";
            return false;
        }
        SchemaField f;
        f.name = name;
        if (typeTok == "f32")
            f.type = SchemaFieldType::F32;
        else if (typeTok == "i32")
            f.type = SchemaFieldType::I32;
        else if (typeTok == "u32")
            f.type = SchemaFieldType::U32;
        else if (typeTok == "bool")
            f.type = SchemaFieldType::Bool;
        else if (typeTok == "vec2")
            f.type = SchemaFieldType::Vec2;
        else if (typeTok == "vec3")
            f.type = SchemaFieldType::Vec3;
        else if (typeTok == "vec4")
            f.type = SchemaFieldType::Vec4;
        else if (typeTok == "str")
            f.type = SchemaFieldType::Str;
        else if (typeTok == "ref")
            f.type = SchemaFieldType::Ref;
        else {
            err = "line " + std::to_string(lineNo) + ": unknown type '" + typeTok + "'";
            return false;
        }
        // The next token is a count or '=' or nothing.
        std::string tok;
        as >> tok;
        if (!tok.empty() && tok != "=") {
            char* end = nullptr;
            errno = 0;
            const unsigned long count = std::strtoul(tok.c_str(), &end, 10);
            if (end == tok.c_str() || *end != '\0' || count == 0 || count > 4096) {
                err = "line " + std::to_string(lineNo) + ": bad array count '" + tok + "'";
                return false;
            }
            f.count = static_cast<uint32_t>(count);
            as >> tok; // the '=' (or nothing)
        }
        // The default literals after '='.
        std::vector<std::string> lits;
        if (tok == "=") {
            if (f.type == SchemaFieldType::Str) {
                // The rest of the line is a quoted string.
                std::string rest;
                std::getline(as, rest);
                size_t rb = rest.find_first_not_of(" \t");
                if (rb == std::string::npos || rest.size() < rb + 2 || rest[rb] != '"' ||
                    rest.back() != '"') {
                    err =
                        "line " + std::to_string(lineNo) + ": str default must be a quoted string";
                    return false;
                }
                lits.push_back(rest.substr(rb + 1, rest.size() - rb - 2));
            } else {
                while (as >> tok)
                    lits.push_back(tok);
            }
        }
        // Encode the default (or zero-fill when absent).
        const uint32_t elemBytes = SchemaTypeStride(f.type);
        const uint32_t total = elemBytes * f.count;
        if (!lits.empty()) {
            // A vecN takes N literals per element; scalars one; str
            // one quoted token.
            uint32_t perElem = 1;
            if (f.type == SchemaFieldType::Vec2)
                perElem = 2;
            else if (f.type == SchemaFieldType::Vec3)
                perElem = 3;
            else if (f.type == SchemaFieldType::Vec4)
                perElem = 4;
            if (f.type == SchemaFieldType::Str) {
                const std::string& s = lits[0];
                if (s.size() > 16ull * 1024ull * 1024ull) {
                    err = "line " + std::to_string(lineNo) + ": str default too long";
                    return false;
                }
                AppendU32(f.defaultValue, static_cast<uint32_t>(s.size()));
                f.defaultValue.insert(f.defaultValue.end(), s.begin(), s.end());
                // Pad the cooked block to 4.
                while (f.defaultValue.size() % 4)
                    f.defaultValue.push_back(0);
            } else {
                if (lits.size() != perElem * f.count) {
                    err = "line " + std::to_string(lineNo) + ": field '" + f.name + "' expects " +
                          std::to_string(perElem * f.count) + " literals, got " +
                          std::to_string(lits.size());
                    return false;
                }
                size_t lit = 0;
                for (uint32_t c = 0; c < f.count; ++c)
                    for (uint32_t k = 0; k < perElem; ++k) {
                        if (!AppendLiteral(lits[lit++], f.type, f.defaultValue)) {
                            err = "line " + std::to_string(lineNo) + ": bad literal '" +
                                  lits[lit - 1] + "'";
                            return false;
                        }
                    }
                if (f.defaultValue.size() != total) {
                    err = "line " + std::to_string(lineNo) + ": internal default encode mismatch";
                    return false;
                }
            }
        } else {
            f.defaultValue.assign(total, 0);
        }
        out.fields.push_back(std::move(f));
    }
    if (!sawHeader) {
        err = "no 'schema <name> <id>' header found";
        return false;
    }
    return FinalizeSchema(out, err);
}

bool ParseNodeSchemaFile(const std::string& text, std::vector<NodeSchema>& out, std::string& err) {
    out.clear();
    // Split on lines that begin a new schema block.
    size_t pos = 0;
    std::vector<std::pair<size_t, size_t>> blocks; // [start, end)
    while (pos < text.size()) {
        const size_t lineEnd = text.find('\n', pos);
        const std::string line =
            text.substr(pos, lineEnd == std::string::npos ? std::string::npos : lineEnd - pos);
        const size_t first = line.find_first_not_of(" \t\r");
        if (first != std::string::npos && line.compare(first, 6, "schema") == 0 &&
            (line.size() == first + 6 || line[first + 6] == ' ' || line[first + 6] == '\t')) {
            blocks.emplace_back(pos, pos); // start; end filled later
            if (blocks.size() > 1)
                blocks[blocks.size() - 2].second = pos;
        }
        pos = lineEnd == std::string::npos ? text.size() : lineEnd + 1;
    }
    if (blocks.empty()) {
        err = "no 'schema' block found";
        return false;
    }
    blocks.back().second = text.size();
    for (const auto& [start, end] : blocks) {
        NodeSchema schema;
        std::string perr;
        if (!ParseNodeSchemaText(text.substr(start, end - start), schema, perr)) {
            err = perr;
            return false;
        }
        out.push_back(std::move(schema));
    }
    return true;
}

// ---- the per-pak registry blob --------------------------------------------

namespace {

constexpr uint32_t kSchemaRegistryMagic = 0x52484353u; // 'SCHR' LE

void PutStr(std::vector<uint8_t>& out, const std::string& s) {
    AppendU32(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

struct RegistryCursor {
    std::span<const uint8_t> b;
    size_t off = 0;
    bool ok = true;
    uint32_t u32() {
        if (!ok || off > b.size() || 4 > b.size() - off) {
            ok = false;
            return 0;
        }
        uint32_t v = 0;
        std::memcpy(&v, b.data() + off, 4);
        off += 4;
        return v;
    }
    std::string str() {
        const uint32_t len = u32();
        if (!ok || len > 4096 || len > b.size() - off) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(b.data() + off), len);
        off += len;
        return s;
    }
    std::vector<uint8_t> bytes(uint32_t len) {
        if (!ok || len > b.size() - off) {
            ok = false;
            return {};
        }
        std::vector<uint8_t> v(b.begin() + off, b.begin() + off + len);
        off += len;
        return v;
    }
};

} // namespace

std::vector<uint8_t> WriteSchemaRegistryBlob(const std::vector<NodeSchema>& schemas) {
    std::vector<uint8_t> out;
    AppendU32(out, kSchemaRegistryMagic);
    AppendU32(out, 1); // version
    AppendU32(out, static_cast<uint32_t>(schemas.size()));
    for (const NodeSchema& s : schemas) {
        AppendU32(out, s.id);
        PutStr(out, s.name);
        AppendU32(out, s.valueBytes);
        AppendU32(out, static_cast<uint32_t>(s.fields.size()));
        for (const SchemaField& f : s.fields) {
            AppendU32(out, f.id);
            PutStr(out, f.name);
            AppendU32(out, static_cast<uint32_t>(f.type));
            AppendU32(out, f.count);
            AppendU32(out, f.offset);
            AppendU32(out, f.stride);
            AppendU32(out, static_cast<uint32_t>(f.defaultValue.size()));
            out.insert(out.end(), f.defaultValue.begin(), f.defaultValue.end());
        }
    }
    return out;
}

bool ParseSchemaRegistryBlob(std::span<const uint8_t> blob,
                             std::vector<NodeSchema>& out,
                             std::string& err) {
    out.clear();
    RegistryCursor c{blob};
    const uint32_t magic = c.u32();
    const uint32_t version = c.u32();
    const uint32_t count = c.u32();
    if (!c.ok || magic != kSchemaRegistryMagic) {
        err = "bad schema registry magic";
        return false;
    }
    if (version != 1) {
        err = "unsupported schema registry version";
        return false;
    }
    if (count > 4096) {
        err = "schema registry count out of range";
        return false;
    }
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        NodeSchema s;
        s.id = c.u32();
        s.name = c.str();
        s.valueBytes = c.u32();
        const uint32_t fieldCount = c.u32();
        if (!c.ok || fieldCount > 4096) {
            err = "schema registry field count out of range";
            return false;
        }
        s.fields.reserve(fieldCount);
        for (uint32_t f = 0; f < fieldCount; ++f) {
            SchemaField fd;
            fd.id = c.u32();
            fd.name = c.str();
            fd.type = static_cast<SchemaFieldType>(c.u32());
            fd.count = c.u32();
            fd.offset = c.u32();
            fd.stride = c.u32();
            const uint32_t defLen = c.u32();
            fd.defaultValue = c.bytes(defLen);
            if (!c.ok)
                break;
            s.fields.push_back(std::move(fd));
        }
        if (!c.ok) {
            err = "schema registry truncated";
            return false;
        }
        out.push_back(std::move(s));
    }
    if (c.off != blob.size()) {
        err = "schema registry trailing bytes";
        return false;
    }
    return true;
}

} // namespace smvk
