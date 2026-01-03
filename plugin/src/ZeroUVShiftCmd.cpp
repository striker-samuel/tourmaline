#include "ZeroUVShiftCmd.h"
#include <maya/MFnMesh.h>
#include <maya/MItMeshPolygon.h>
#include <maya/MSelectionList.h>
#include <maya/MGlobal.h>
#include <maya/MSyntax.h>
#include <maya/MObject.h>
#include <maya/MDagPath.h>
#include <maya/MFloatVector.h>
#include <maya/MArgDatabase.h>

#include <cmath>
#include <unordered_set>

#include <map>
#include <vector>

ZeroUVShiftCmd::ZeroUVShiftCmd() {}
ZeroUVShiftCmd::~ZeroUVShiftCmd() {}

void* ZeroUVShiftCmd::creator() {
    return new ZeroUVShiftCmd();
}


MSyntax ZeroUVShiftCmd::newSyntax() {
    MSyntax syntax;
    syntax.addFlag("-ofs", "-offset", MSyntax::kDouble);
    syntax.addFlag("-flp", "-flip", MSyntax::kBoolean);
    syntax.addFlag("-fbp", "-flipByPatch", MSyntax::kBoolean);
    syntax.addFlag("-uvs", "-uvSetName", MSyntax::kString);

    return syntax;
}


struct UVKey {
    MPoint a, b;

    UVKey(const MPoint& a_, const MPoint& b_) {
        a = a_;
        b = b_;
    }

    bool operator<(const UVKey& other) const {
        if (a.x != other.a.x) return a.x < other.a.x;
        if (a.y != other.a.y) return a.y < other.a.y;
        if (b.x != other.b.x) return b.x < other.b.x;
        return b.y < other.b.y;
    }
};

MStatus ZeroUVShiftCmd::doIt(const MArgList& args) {
    MStatus status;

    MArgDatabase argData(syntax(), args);

    if (argData.isFlagSet("-ofs")) {
        argData.getFlagArgument("-ofs", 0, offset);
    }

    MString uvSetName = "map1";
    if (argData.isFlagSet("-uvs")) {
        argData.getFlagArgument("-uvs", 0, uvSetName);
    }

    bool flip = false;
    if (argData.isFlagSet("-flp")) {
        argData.getFlagArgument("-flp", 0, flip);
    }

    bool flipByPatch = false;
    if (argData.isFlagSet("-fbp")) {
        argData.getFlagArgument("-fbp", 0, flipByPatch);
    }

    // Get selected mesh
    MSelectionList sel;
    MGlobal::getActiveSelectionList(sel);
    MDagPath meshPath;
    sel.getDagPath(0, meshPath);
    meshPath.extendToShape();

    MFnMesh meshFn(meshPath, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    std::unordered_set<int> seenUVIds;

    // Iterate over faces
    MItMeshPolygon faceIt(meshPath, MObject::kNullObj);

    std::map<UVKey, std::vector<int>> faceGroups;

    for (; !faceIt.isDone(); faceIt.next()) {
        double uvArea = 0.0;
        faceIt.getUVArea(uvArea, &uvSetName);
        if (uvArea >= 1e-7) continue;

        int faceId = faceIt.index();

        int uvId0 = -1, uvId1 = -1;
        float u0, v0, u1, v1;


        if (faceIt.getUVIndex(0, uvId0, u0, v0, &uvSetName) != MS::kSuccess) continue;
        if (faceIt.getUVIndex(1, uvId1, u1, v1, &uvSetName) != MS::kSuccess) continue;

        MPoint uvA(u0, v0);
        MPoint uvB(u1, v1);

        faceGroups[UVKey(uvA, uvB)].push_back(faceId);
    }

    //debugDumpFaceGroups(faceGroups);

    for (const auto& [uvPair, faceList] : faceGroups) {

        MFloatVector AB(
            static_cast<float>(uvPair.b.x - uvPair.a.x),
            static_cast<float>(uvPair.b.y - uvPair.a.y),
            0.0f
        );
        AB.normalize();

        MFloatVector normal;
        bool will_flip = flip;

        if (flipByPatch){
            // if (static_cast<int>(uvPair.a.y) % 2 == 1){
            if (static_cast<int>(uvPair.a.y) < 0 || static_cast<int>(uvPair.a.x) < 0 ){
                will_flip = !will_flip;
            }   
        }

        if (will_flip){
            normal = {-AB.y, AB.x, 0.0f};
        }
        else{
            normal = {AB.y, -AB.x, 0.0f};
        }

        int count = 1;
        int dummyIndex;

        for (int faceId : faceList) {

            faceIt.setIndex(faceId, dummyIndex);

            int uvId2 = -1, uvId3 = -1;

            faceIt.getUVIndex(0, uvId2, &uvSetName);
            faceIt.getUVIndex(1, uvId3, &uvSetName);

            for (int uvId : { uvId2, uvId3 }) {
                float oldU, oldV;
                meshFn.getUV(uvId, oldU, oldV, &uvSetName);

                //if (seenUVIds.insert(uvId).second) {
                tweakedUVIds.append(uvId);
                uOld.append(oldU);
                vOld.append(oldV);
                //}

                uOffsets.append(normal.x * (offset * count));
                vOffsets.append(normal.y * (offset * count));
            }

            count++;

        }
    }

    

    return redoIt();
}


MStatus ZeroUVShiftCmd::redoIt() {
    MStatus status;

    MSelectionList sel;
    MGlobal::getActiveSelectionList(sel);
    MDagPath meshPath;
    sel.getDagPath(0, meshPath);
    meshPath.extendToShape();

    MFnMesh meshFn(meshPath, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    MFloatArray newUs, newVs;

    for (unsigned int i = 0; i < tweakedUVIds.length(); ++i) {
        newUs.append(uOld[i] + uOffsets[i]);
        newVs.append(vOld[i] + vOffsets[i]);
    }

    status = meshFn.setSomeUVs(tweakedUVIds, newUs, newVs, &uvSetName);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    meshFn.updateSurface();
    return MS::kSuccess;
}



MStatus ZeroUVShiftCmd::undoIt() {
    MStatus status;

    MSelectionList sel;
    MGlobal::getActiveSelectionList(sel);
    MDagPath meshPath;
    sel.getDagPath(0, meshPath);
    meshPath.extendToShape();

    MFnMesh meshFn(meshPath, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);


    meshFn.setSomeUVs(tweakedUVIds, uOld, vOld, &uvSetName);

    return MS::kSuccess;
}
