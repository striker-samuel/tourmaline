#pragma once

#include <maya/MPxNode.h>
#include <maya/MTypeId.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MFnNumericAttribute.h>

class UVZeroAreaNode : public MPxNode {
public:
    UVZeroAreaNode() = default;
    virtual ~UVZeroAreaNode() = default;

    static void* creator();
    static MStatus initialize();
    MStatus compute(const MPlug& plug, MDataBlock& data) override;

    static MTypeId id;

    static MObject inputMesh;
    static MObject outputFaceIndices; // indices of faces in zero-area UV shells
    static MObject outputFaceComponents; // indices of faces in zero-area UV shells
    static MObject uvSetNameAttr; // uv map name
    static MObject perShellComponentsAttr; 
    //static MObject perShellUvIdsAttr;
};
