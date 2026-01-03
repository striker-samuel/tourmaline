#pragma once

#include <maya/MPxCommand.h>
#include <maya/MStatus.h>

class UVThicknessFix : public MPxCommand {
public:
    UVThicknessFix() = default;
    virtual ~UVThicknessFix() = default;

    static void* creator();
    MStatus doIt(const MArgList& args) override;
};
