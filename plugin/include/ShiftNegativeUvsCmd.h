#pragma once

#include <maya/MPxCommand.h>
#include <maya/MString.h>
#include <maya/MDagPath.h>
#include <maya/MFloatArray.h>
#include <maya/MIntArray.h>

class ShiftNegativeUVsCmd : public MPxCommand {
public:
    ShiftNegativeUVsCmd() = default;
    ~ShiftNegativeUVsCmd() override = default;

    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    MStatus redoIt() override;
    MStatus undoIt() override;
    bool isUndoable() const override;

private:
    MStatus parseArgs(const MArgList& args);

    MString uvSetName;
    MDagPath meshPath;

    // UVs that were modified
    MIntArray tweakedUVIds;
    MFloatArray uOld;
    MFloatArray vOld;
    MFloatArray uNew;
    MFloatArray vNew;
};
