#include "ShiftNegativeUvsCmd.h"

#include <maya/MSelectionList.h>
#include <maya/MGlobal.h>
#include <maya/MArgDatabase.h>
#include <maya/MArgList.h>
#include <maya/MSyntax.h>
#include <maya/MItMeshPolygon.h>
#include <maya/MFnMesh.h>
#include <maya/MFloatArray.h>
#include <maya/MIntArray.h>
#include <cmath>
#include <unordered_set>

void* ShiftNegativeUVsCmd::creator() {
    return new ShiftNegativeUVsCmd();
}

MSyntax ShiftNegativeUVsCmd::newSyntax() {
    MSyntax syntax;
    syntax.addArg(MSyntax::kString); // mesh name
    syntax.addFlag("-uvs", "-uvSetName", MSyntax::kString);
    return syntax;
}

MStatus ShiftNegativeUVsCmd::parseArgs(const MArgList& args) {
    MArgDatabase argData(newSyntax(), args);

    if (argData.isFlagSet("-uvs")) {
        argData.getFlagArgument("-uvs", 0, uvSetName);
    } else {
        uvSetName = MString("map1"); // default
    }

    MString meshName;
    argData.getCommandArgument(0, meshName);

    MSelectionList sel;
    CHECK_MSTATUS_AND_RETURN_IT(sel.add(meshName));
    CHECK_MSTATUS_AND_RETURN_IT(sel.getDagPath(0, meshPath));
    meshPath.extendToShape();

    return MS::kSuccess;
}

MStatus ShiftNegativeUVsCmd::doIt(const MArgList& args) {
    
    MStatus status;

    CHECK_MSTATUS_AND_RETURN_IT(parseArgs(args));

    uOld.clear();
    vOld.clear();
    tweakedUVIds.clear();

    MItMeshPolygon faceIt(meshPath, MObject::kNullObj);

    std::unordered_set<int> modifiedUVs;
    for (; !faceIt.isDone(); faceIt.next()) {
        int vertexCount = faceIt.polygonVertexCount();
        for (int i = 0; i < vertexCount; ++i) {
            float u, v;
            int uvIdx;
            if (faceIt.getUVIndex(i, uvIdx, u, v, &uvSetName) != MS::kSuccess)
                continue;
    
            if (modifiedUVs.count(uvIdx) > 0)
                continue;
    
            // int offsetU = (u < 0.0f) ? -(std::floor(u)) : 0;
            // int offsetV = (v < 0.0f) ? -(std::floor(v)) : 0;

            int tileU = std::floor(u);
            float offsetU = (u < 0.0f) ? -2.0f * tileU -1.0f : 0.0f;

            int tileV = std::floor(v);
            float offsetV = (v < 0.0f) ? -2.0f * tileV -1.0f  : 0.0f;

            tweakedUVIds.append(uvIdx);
            uOld.append(u);
            vOld.append(v);
            uNew.append(u + static_cast<float>(offsetU));
            vNew.append(v + static_cast<float>(offsetV));
            modifiedUVs.insert(uvIdx);
        }
    }
    MGlobal::displayInfo(MString("Shifted ") + tweakedUVIds.length() + " UVs to positive space.");

    redoIt();

}

MStatus ShiftNegativeUVsCmd::undoIt() {
    MStatus status;
    MFnMesh meshFn(meshPath, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    CHECK_MSTATUS_AND_RETURN_IT(meshFn.setSomeUVs(tweakedUVIds, uOld, vOld, &uvSetName));
    meshFn.updateSurface();
    return MS::kSuccess;
}

MStatus ShiftNegativeUVsCmd::redoIt() {
    MStatus status;
    MFnMesh meshFn(meshPath, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);
    CHECK_MSTATUS_AND_RETURN_IT(meshFn.setSomeUVs(tweakedUVIds, uNew, vNew, &uvSetName));

    meshFn.updateSurface();
    return MS::kSuccess;
}

bool ShiftNegativeUVsCmd::isUndoable() const {
    return true;
}
