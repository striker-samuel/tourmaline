#include "UVScalerNode.h"
#include <maya/MFnMeshData.h>
#include <maya/MFnMesh.h>
#include <maya/MGlobal.h>

MTypeId UVScalerNode::id(0x0013B1C0);  // Change this to your unique ID

MObject UVScalerNode::inputMesh;
MObject UVScalerNode::scale;
MObject UVScalerNode::outputMesh;

void* UVScalerNode::creator() {
    return new UVScalerNode();
}

MStatus UVScalerNode::initialize() {
    MFnTypedAttribute tAttr;
    MFnNumericAttribute nAttr;

    inputMesh = tAttr.create("inputMesh", "inMesh", MFnData::kMesh);
    tAttr.setStorable(true);
    tAttr.setReadable(false);
    tAttr.setWritable(true);
    addAttribute(inputMesh);

    scale = nAttr.create("uvScale", "uvs", MFnNumericData::kFloat, 1.0);
    nAttr.setKeyable(true);
    nAttr.setStorable(true);
    nAttr.setReadable(true);
    nAttr.setWritable(true);
    addAttribute(scale);

    outputMesh = tAttr.create("outputMesh", "outMesh", MFnData::kMesh);
    tAttr.setStorable(false);
    tAttr.setWritable(false);
    tAttr.setReadable(true);
    addAttribute(outputMesh);

    attributeAffects(inputMesh, outputMesh);
    attributeAffects(scale, outputMesh);

    return MS::kSuccess;
}

MStatus UVScalerNode::compute(const MPlug& plug, MDataBlock& data) {
    if (plug != outputMesh)
        return MS::kUnknownParameter;

    MStatus status;

    MDataHandle inputMeshHandle = data.inputValue(inputMesh, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    MObject inMeshObj = inputMeshHandle.asMesh();
    if (inMeshObj.isNull()) return MS::kFailure;

    float scaleFactor = data.inputValue(scale).asFloat();

    // Create output mesh data
    MFnMeshData meshDataCreator;
    MObject newOutputData = meshDataCreator.create(&status);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    MFnMesh inMesh(inMeshObj);
    MObject outMeshObj = inMesh.copy(inMeshObj, newOutputData, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    MFnMesh outMesh(outMeshObj, &status);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    // Modify UVs
    MFloatArray uArray, vArray;
    status = outMesh.getUVs(uArray, vArray);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    for (unsigned int i = 0; i < uArray.length(); ++i) {
        uArray[i] *= scaleFactor;
        vArray[i] *= scaleFactor;
    }

    status = outMesh.setUVs(uArray, vArray);
    CHECK_MSTATUS_AND_RETURN_IT(status);

    data.outputValue(outputMesh).set(newOutputData);
    data.setClean(plug);

    return MS::kSuccess;
}
