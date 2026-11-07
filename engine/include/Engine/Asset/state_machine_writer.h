// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Canonical, lossless authoring of the versioned StateMachine grammar.
// The runtime parser remains the authority: callers should reparse the
// serialized text before writing it to disk.
#include <Engine/Asset/state_machine.h>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>

namespace tour::asset {

inline std::string SerializeStateMachineText(const StateMachineAsset& doc) {
    if (doc.entry >= doc.states.size())
        return {};
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << "statemachine 1\n";
    for (const auto& p : doc.parameters) {
        out << "param " << p.name << ' ';
        switch (p.kind) {
        case StateParamKind::Bool:
            out << "bool " << (p.initial != 0 ? "true" : "false");
            break;
        case StateParamKind::Integer:
            out << "int " << int32_t(p.initial);
            break;
        case StateParamKind::Float:
            out << "float " << p.initial;
            break;
        case StateParamKind::Trigger:
            out << "trigger";
            break;
        }
        out << '\n';
    }
    for (const auto& state : doc.states)
        out << "state " << state << '\n';
    out << "entry " << doc.states[doc.entry] << '\n';
    for (const auto& t : doc.transitions) {
        if (t.to >= doc.states.size() || t.parameter >= doc.parameters.size() ||
            (t.from != UINT32_MAX && t.from >= doc.states.size()))
            return {};
        out << "transition ";
        if (t.from == UINT32_MAX)
            out << '*';
        else
            out << doc.states[t.from];
        out << ' ' << doc.states[t.to] << ' ' << doc.parameters[t.parameter].name << ' ';
        switch (t.compare) {
        case StateCompare::Equal:
            out << "==";
            break;
        case StateCompare::NotEqual:
            out << "!=";
            break;
        case StateCompare::Less:
            out << "<";
            break;
        case StateCompare::LessEqual:
            out << "<=";
            break;
        case StateCompare::Greater:
            out << ">";
            break;
        case StateCompare::GreaterEqual:
            out << ">=";
            break;
        case StateCompare::Fired:
            out << "fired";
            break;
        }
        if (t.compare != StateCompare::Fired) {
            if (doc.parameters[t.parameter].kind == StateParamKind::Bool)
                out << ' ' << (t.threshold != 0 ? "true" : "false");
            else if (doc.parameters[t.parameter].kind == StateParamKind::Integer)
                out << ' ' << int32_t(t.threshold);
            else
                out << ' ' << t.threshold;
        }
        out << '\n';
    }
    return out.str();
}

} // namespace tour::asset
