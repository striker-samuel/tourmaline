// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Build-time SPIR-V -> constexpr C++ shader interface catalog.
// This tool intentionally has no Vulkan device or engine dependency.
#include <spirv_reflect.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace {
struct Descriptor {
    uint32_t set = 0, binding = 0, count = 0, type = 0;
};
struct Range {
    uint32_t offset = 0, size = 0;
};
struct Spec {
    uint32_t id = 0, size = 0;
};
struct Input {
    uint32_t location = 0, components = 0;
};
struct Module {
    std::string symbol, path, entry, stage;
    uint64_t id = 0;
    std::vector<Descriptor> descriptors;
    std::vector<Range> push;
    std::vector<Spec> specs;
    std::vector<Input> inputs;
};

std::string Escape(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    return out + "\"";
}

uint64_t StableId(const std::string& path) {
    uint64_t hash = 14695981039346656037ULL; // FNV-1a64
    for (unsigned char c : path) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string Symbol(const fs::path& path) {
    std::string name = path.filename().generic_string();
    if (name.ends_with(".spv"))
        name.resize(name.size() - 4);
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
    }
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])))
        name.insert(name.begin(), '_');
    return name;
}

std::string Stage(SpvReflectShaderStageFlagBits stage) {
    switch (stage) {
    case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:
        return "Vertex";
    case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT:
        return "Fragment";
    case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT:
        return "Compute";
    case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT:
        return "Geometry";
    case SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
        return "TessControl";
    case SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
        return "TessEvaluation";
    case SPV_REFLECT_SHADER_STAGE_TASK_BIT_EXT:
        return "Task";
    case SPV_REFLECT_SHADER_STAGE_MESH_BIT_EXT:
        return "Mesh";
    default:
        return "Other";
    }
}

bool ReadModule(const fs::path& file, const fs::path& root, Module& out, std::string& error) {
    std::error_code ec;
    const fs::path rel = fs::relative(file, root, ec);
    if (ec || rel.empty() || rel.is_absolute() || *rel.begin() == "..") {
        error = "shader output must be inside runtime root: " + file.generic_string();
        return false;
    }
    out.path = rel.generic_string();
    out.symbol = Symbol(file);
    out.id = StableId(out.path);

    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "could not open: " + file.generic_string();
        return false;
    }
    const auto size = in.tellg();
    if (size <= 0 || (size % 4) != 0) {
        error = "invalid SPIR-V byte count: " + file.generic_string();
        return false;
    }
    std::vector<char> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        error = "could not read: " + file.generic_string();
        return false;
    }
    SpvReflectShaderModule reflect{};
    const SpvReflectResult created =
        spvReflectCreateShaderModule(bytes.size(), bytes.data(), &reflect);
    if (created != SPV_REFLECT_RESULT_SUCCESS) {
        error =
            "SPIR-V reflection failed (" + std::to_string(created) + "): " + file.generic_string();
        return false;
    }
    out.stage = Stage(reflect.shader_stage);
    if (reflect.entry_point_count && reflect.entry_points[0].name)
        out.entry = reflect.entry_points[0].name;

    uint32_t count = 0;
    if (spvReflectEnumerateDescriptorBindings(&reflect, &count, nullptr) ==
        SPV_REFLECT_RESULT_SUCCESS) {
        std::vector<SpvReflectDescriptorBinding*> bindings(count);
        if (spvReflectEnumerateDescriptorBindings(&reflect, &count, bindings.data()) ==
            SPV_REFLECT_RESULT_SUCCESS) {
            for (auto* b : bindings) {
                if (b)
                    out.descriptors.push_back(
                        {b->set, b->binding, b->count, static_cast<uint32_t>(b->descriptor_type)});
            }
        }
    }
    count = 0;
    if (spvReflectEnumeratePushConstantBlocks(&reflect, &count, nullptr) ==
        SPV_REFLECT_RESULT_SUCCESS) {
        std::vector<SpvReflectBlockVariable*> blocks(count);
        if (spvReflectEnumeratePushConstantBlocks(&reflect, &count, blocks.data()) ==
            SPV_REFLECT_RESULT_SUCCESS) {
            for (auto* b : blocks) {
                if (b && b->size)
                    out.push.push_back({b->offset, b->size});
            }
        }
    }
    count = 0;
    if (spvReflectEnumerateSpecializationConstants(&reflect, &count, nullptr) ==
        SPV_REFLECT_RESULT_SUCCESS) {
        std::vector<SpvReflectSpecializationConstant*> specs(count);
        if (spvReflectEnumerateSpecializationConstants(&reflect, &count, specs.data()) ==
            SPV_REFLECT_RESULT_SUCCESS) {
            for (auto* s : specs) {
                if (s)
                    out.specs.push_back({s->constant_id, s->default_value_size});
            }
        }
    }
    if (reflect.shader_stage == SPV_REFLECT_SHADER_STAGE_VERTEX_BIT) {
        count = 0;
        if (spvReflectEnumerateInputVariables(&reflect, &count, nullptr) ==
            SPV_REFLECT_RESULT_SUCCESS) {
            std::vector<SpvReflectInterfaceVariable*> vars(count);
            if (spvReflectEnumerateInputVariables(&reflect, &count, vars.data()) ==
                SPV_REFLECT_RESULT_SUCCESS) {
                for (auto* v : vars) {
                    if (v && !(v->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN))
                        out.inputs.push_back({v->location, v->numeric.vector.component_count});
                }
            }
        }
    }
    spvReflectDestroyShaderModule(&reflect);

    std::sort(out.descriptors.begin(), out.descriptors.end(), [](auto a, auto b) {
        return std::tie(a.set, a.binding) < std::tie(b.set, b.binding);
    });
    std::sort(out.push.begin(), out.push.end(), [](auto a, auto b) { return a.offset < b.offset; });
    std::sort(out.specs.begin(), out.specs.end(), [](auto a, auto b) { return a.id < b.id; });
    std::sort(out.inputs.begin(), out.inputs.end(), [](auto a, auto b) {
        return a.location < b.location;
    });
    return true;
}

template <class T, class Print>
void EmitArray(std::ostringstream& out,
               const std::string& type,
               const std::string& name,
               const std::vector<T>& values,
               Print print) {
    out << "inline constexpr std::array<" << type << ", " << values.size() << "> " << name
        << "{{\n";
    for (const auto& v : values) {
        out << "    " << type << "{";
        print(out, v);
        out << "},\n";
    }
    out << "}};\n";
}

std::string Generate(const std::vector<Module>& modules) {
    std::ostringstream out;
    out << "// GENERATED by tourmaline_shader_header; DO NOT EDIT.\n"
           "// Compile-time metadata for project-local compiled SPIR-V modules.\n"
           "// StableId hashes the executable-relative SPIR-V path; it is NOT\n"
           "// an assets.db ShaderKey and must not enter the pak catalog ABI.\n"
           "#pragma once\n"
           "#include <array>\n#include <cstdint>\n#include <span>\n"
           "#include <string_view>\n\n"
           "namespace project::compiled_shaders {\n"
           "enum class Stage : std::uint8_t {\n"
           "    Vertex, Fragment, Compute, Geometry, TessControl,\n"
           "    TessEvaluation, Task, Mesh, Other\n};\n"
           "struct DescriptorBinding {\n"
           "    std::uint32_t set, binding, count, type;\n"
           "};\n"
           "struct PushConstantRange { std::uint32_t offset, size; };\n"
           "struct SpecializationConstant { std::uint32_t id, size; };\n"
           "struct VertexInput { std::uint32_t location, components; };\n"
           "struct ShaderInfo {\n"
           "    std::uint64_t stableId;\n"
           "    std::string_view path, entryPoint;\n"
           "    Stage stage;\n"
           "    std::span<const DescriptorBinding> descriptors;\n"
           "    std::span<const PushConstantRange> pushConstants;\n"
           "    std::span<const SpecializationConstant> specializationConstants;\n"
           "    std::span<const VertexInput> vertexInputs;\n"
           "};\n\nnamespace detail {\n";

    for (const auto& m : modules) {
        const auto prefix = m.symbol + "_";
        EmitArray(
            out, "DescriptorBinding", prefix + "descriptors", m.descriptors, [](auto& s, auto v) {
                s << v.set << ", " << v.binding << ", " << v.count << ", " << v.type;
            });
        EmitArray(out, "PushConstantRange", prefix + "push", m.push, [](auto& s, auto v) {
            s << v.offset << ", " << v.size;
        });
        EmitArray(out, "SpecializationConstant", prefix + "specs", m.specs, [](auto& s, auto v) {
            s << v.id << ", " << v.size;
        });
        EmitArray(out, "VertexInput", prefix + "inputs", m.inputs, [](auto& s, auto v) {
            s << v.location << ", " << v.components;
        });
    }
    out << "} // namespace detail\n\n";
    for (const auto& m : modules) {
        const auto prefix = m.symbol + "_";
        out << "inline constexpr ShaderInfo " << m.symbol << "{\n"
            << "    0x" << std::hex << m.id << std::dec << "ULL,\n"
            << "    " << Escape(m.path) << ", " << Escape(m.entry) << ", Stage::" << m.stage
            << ",\n"
            << "    detail::" << prefix << "descriptors,\n"
            << "    detail::" << prefix << "push,\n"
            << "    detail::" << prefix << "specs,\n"
            << "    detail::" << prefix << "inputs,\n"
            << "};\n";
    }
    out << "} // namespace project::compiled_shaders\n";
    return out.str();
}

bool WriteIfChanged(const fs::path& output, const std::string& contents, std::string& error) {
    std::ifstream previous(output, std::ios::binary);
    if (previous) {
        std::ostringstream buf;
        buf << previous.rdbuf();
        if (buf.str() == contents)
            return true;
    }
    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    if (ec) {
        error = "cannot create header directory: " + ec.message();
        return false;
    }
    std::ofstream next(output, std::ios::binary | std::ios::trunc);
    if (!next || !(next << contents)) {
        error = "cannot write generated header: " + output.generic_string();
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    fs::path output, runtimeRoot;
    std::vector<fs::path> files;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "usage: tourmaline_shader_header --output header"
                         " --runtime-root exe-directory --shader file.spv [...]\n";
            return 0;
        }
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << arg << '\n';
            return 2;
        }
        const std::string value = argv[++i];
        if (arg == "--output")
            output = value;
        else if (arg == "--runtime-root")
            runtimeRoot = value;
        else if (arg == "--shader")
            files.emplace_back(value);
        else {
            std::cerr << "unknown option: " << arg << '\n';
            return 2;
        }
    }
    if (output.empty() || runtimeRoot.empty() || files.empty()) {
        std::cerr << "usage: tourmaline_shader_header --output header"
                     " --runtime-root exe-directory --shader file.spv [...]\n";
        return 2;
    }
    std::vector<Module> modules;
    std::set<std::string> symbols, paths;
    for (const auto& file : files) {
        Module m;
        std::string error;
        if (!ReadModule(file, runtimeRoot, m, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        if (!symbols.insert(m.symbol).second || !paths.insert(m.path).second) {
            std::cerr << "duplicate shader name or output path: " << m.path << '\n';
            return 1;
        }
        modules.push_back(std::move(m));
    }
    std::sort(modules.begin(), modules.end(), [](const Module& a, const Module& b) {
        return a.path < b.path;
    });
    std::string error;
    if (!WriteIfChanged(output, Generate(modules), error)) {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}
