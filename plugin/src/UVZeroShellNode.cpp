#include "UVZeroShellNode.h"
#include <maya/MFnMesh.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MFnNumericAttribute.h>
#include <maya/MFnMeshData.h>
#include <maya/MItMeshPolygon.h>
#include <maya/MFloatArray.h>
#include <maya/MPointArray.h>
#include <maya/MIntArray.h>
#include <maya/MFnStringData.h>
#include <maya/MFnIntArrayData.h>
#include <maya/MFnComponentListData.h>
#include <maya/MFnSingleIndexedComponent.h>
#include <maya/MArrayDataBuilder.h>
#include <maya/MGlobal.h>

#include <vector>
#include <map>
#include <set>

MTypeId UVZeroAreaNode::id(0x0013B1C1);  // Change this to a unique ID

MObject UVZeroAreaNode::inputMesh;
MObject UVZeroAreaNode::outputFaceIndices;
MObject UVZeroAreaNode::outputFaceComponents;
MObject UVZeroAreaNode::uvSetNameAttr;
MObject UVZeroAreaNode::perShellComponentsAttr;

void* UVZeroAreaNode::creator() {
    return new UVZeroAreaNode();
}

MStatus UVZeroAreaNode::initialize() {
    MFnTypedAttribute tAttr;
    MFnNumericAttribute nAttr;

    inputMesh = tAttr.create("inputMesh", "inMesh", MFnData::kMesh);
    tAttr.setReadable(false);
    tAttr.setWritable(true);
    tAttr.setStorable(true);
    addAttribute(inputMesh);

    outputFaceIndices = tAttr.create("zeroAreaFaces", "zaf", MFnData::kIntArray);
    tAttr.setWritable(false);
    tAttr.setReadable(true);
    tAttr.setStorable(false);
    addAttribute(outputFaceIndices);

    outputFaceComponents = tAttr.create("outputComponents", "outComp", MFnData::kComponentList);
    tAttr.setWritable(false);
    tAttr.setReadable(true);
    tAttr.setStorable(false);
    addAttribute(outputFaceComponents);

    MFnStringData defaultUvSetData;
    MObject defaultString = defaultUvSetData.create("map1");

    uvSetNameAttr = tAttr.create("uvSetName", "uvSet", MFnData::kString, defaultString);
    tAttr.setWritable(true);
    tAttr.setReadable(true);
    tAttr.setStorable(true);
    addAttribute(uvSetNameAttr);

    perShellComponentsAttr = tAttr.create("perShellComponents", "psc", MFnData::kComponentList);
    tAttr.setArray(true);
    tAttr.setUsesArrayDataBuilder(true);
    tAttr.setReadable(true);
    tAttr.setWritable(false);
    tAttr.setStorable(false);
    addAttribute(perShellComponentsAttr);

    

    attributeAffects(inputMesh, perShellComponentsAttr);
    attributeAffects(uvSetNameAttr, perShellComponentsAttr);

    attributeAffects(inputMesh, outputFaceIndices);
    attributeAffects(inputMesh, outputFaceComponents);

    attributeAffects(uvSetNameAttr, outputFaceIndices);
    attributeAffects(uvSetNameAttr, outputFaceComponents);

    return MS::kSuccess;
}

MStatus UVZeroAreaNode::compute(const MPlug& plug, MDataBlock& data) {
    if (plug != outputFaceIndices &&
        plug != outputFaceComponents &&
        plug != perShellComponentsAttr)
        return MS::kUnknownParameter;

    // Input mesh
    MDataHandle meshHandle = data.inputValue(inputMesh);
    MObject meshObj = meshHandle.asMesh();
    if (meshObj.isNull()) return MS::kFailure;
    MFnMesh meshFn(meshObj);

    // UV set name
    MDataHandle uvSetHandle = data.inputValue(uvSetNameAttr);
    MString uvSetName = uvSetHandle.asString();

    // UV shell info
    MIntArray uvToShell;
    unsigned int numShells = 0;
    meshFn.getUvShellsIds(uvToShell, numShells, &uvSetName);

    // Core data collections
    MIntArray zeroFaces;
    std::map<int, int> faceIdToShellId;
    std::map<int, MIntArray> shellIdToZeroFaces;
    std::map<int, MIntArray> shellIdToZeroUVids;

    std::map<int, std::pair<float, float>> faceIdToUV;

    // Single pass over all faces
    MItMeshPolygon faceIt(meshObj);
    for (; !faceIt.isDone(); faceIt.next()) {
        int faceId = faceIt.index();
        double uvArea = 0.0;
        faceIt.getUVArea(uvArea, &uvSetName);

        if (uvArea >= 1e-7) {
            continue;
        }

        int shellId = -1;
        int numVerts = faceIt.polygonVertexCount();

        for (int i = 0; i < numVerts; ++i) {
            int uvId = -1;
            
            float uValue;
            float vValue;

            if (faceIt.getUVIndex(i, uvId, uValue, vValue, &uvSetName) == MS::kSuccess &&
                uvId >= 0 && uvId < (int)uvToShell.length()) {
                shellId = uvToShell[uvId];
                faceIdToShellId[faceId] = shellId;
                faceIdToUV[faceId] = std::pair{ uValue, vValue };
                break;
            }
        }

        if (shellId != -1 && uvArea < 1e-7) {
            zeroFaces.append(faceId);
            shellIdToZeroFaces[shellId].append(faceId);
        }
    }

    // Output: zeroAreaFaces (zaf)
    {
        MDataHandle indicesHandle = data.outputValue(outputFaceIndices);
        MFnIntArrayData intArrayFn;
        MObject intArrayObj = intArrayFn.create(zeroFaces);
        indicesHandle.set(intArrayObj);
        data.setClean(outputFaceIndices);
    }

    // Output: outputComponents
    {
        MDataHandle compHandle = data.outputValue(outputFaceComponents);
        MFnComponentListData compListFn;
        MObject compListObj = compListFn.create();

        for (auto& [shellId, faceList] : shellIdToZeroFaces) {
            MFnSingleIndexedComponent faceCompFn;
            MObject faceComp = faceCompFn.create(MFn::kMeshPolygonComponent);
            faceCompFn.addElements(faceList);
            compListFn.add(faceComp);
        }

        compHandle.set(compListObj);
        data.setClean(outputFaceComponents);
    }

    // Output: perShellComponents (psc)
    {
        MArrayDataHandle arrayHandle = data.outputArrayValue(perShellComponentsAttr);
        MArrayDataBuilder arrayBuilder(perShellComponentsAttr, static_cast<unsigned int>(shellIdToZeroFaces.size()));

        for (auto& [shellId, faceList] : shellIdToZeroFaces) {
            MFnComponentListData shellListFn;
            MObject shellListObj = shellListFn.create();

            MFnSingleIndexedComponent faceCompFn;
            MObject faceComp = faceCompFn.create(MFn::kMeshPolygonComponent);
            faceCompFn.addElements(faceList);
            shellListFn.add(faceComp);

            MDataHandle shellHandle = arrayBuilder.addElement(shellId);
            shellHandle.set(shellListObj);
        }

        arrayHandle.set(arrayBuilder);
        arrayHandle.setAllClean();
    }

    return MS::kSuccess;
}
