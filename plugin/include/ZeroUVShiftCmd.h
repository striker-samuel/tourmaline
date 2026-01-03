#pragma once

#include <maya/MPxCommand.h>
#include <maya/MArgList.h>
#include <maya/MIntArray.h>
#include <maya/MFloatArray.h>
#include <maya/MString.h>


class ZeroUVShiftCmd : public MPxCommand {
public:
    ZeroUVShiftCmd();
    ~ZeroUVShiftCmd() override;

    static void* creator();
    static MSyntax newSyntax();

    MStatus doIt(const MArgList& args) override;
    MStatus redoIt() override;
    MStatus undoIt() override;

private:

    MString uvSetName = "map1";
    double offset = 0.003f;  // default value
    bool flip = false;
    bool flipByPatch = false;

    MIntArray tweakedUVIds;
    MFloatArray uOld, vOld;     // original values
    MFloatArray uOffsets, vOffsets;  // offsets applied

};
