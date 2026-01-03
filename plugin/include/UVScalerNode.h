#pragma once

#include <maya/MPxNode.h>
#include <maya/MTypeId.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MFnNumericAttribute.h>
#include <maya/MFloatArray.h>
#include <maya/MFloatVector.h>
//#include <maya/MItMeshUV.h>
#include <maya/MFnMesh.h>

class UVScalerNode : public MPxNode {
public:
    UVScalerNode() = default;
    virtual ~UVScalerNode() = default;

    static void* creator();
    static MStatus initialize();

    MStatus compute(const MPlug& plug, MDataBlock& data) override;

    static MTypeId id;

    static MObject inputMesh;
    static MObject scale;
    static MObject outputMesh;
    static MObject inputUVSet;
    static MObject outputUVSet;

};
