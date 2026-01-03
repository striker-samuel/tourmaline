#include <maya/MFnMesh.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MFnStringData.h>
#include <maya/MFnNumericAttribute.h>
#include <maya/MFnComponentListData.h>
#include <maya/MFnSingleIndexedComponent.h>
#include <maya/MItMeshFaceVertex.h>
#include <maya/MArrayDataBuilder.h>
#include <maya/MItMeshPolygon.h>
#include <maya/MFloatVector.h>
#include <maya/MPoint.h>
#include <maya/MGlobal.h>

#include "UVShellExpandNode.h"

#include <numeric>
#include <cmath>
#include <unordered_set>
#include <vector>

MTypeId UVShellExpandNode::id(0x0013B1C2);  // Unique ID for the node

// Attributes
MObject UVShellExpandNode::inputShellComponents;
MObject UVShellExpandNode::uvSetNameAttr;
MObject UVShellExpandNode::offsetAttr;



void* UVShellExpandNode::creator() {
   return new UVShellExpandNode();
}


MStatus UVShellExpandNode::initialize() {
   MFnTypedAttribute tAttr;
   MFnNumericAttribute nAttr;

   inputShellComponents = tAttr.create("inputShellComponents", "isc", MFnData::kComponentList);
   tAttr.setArray(true);
   tAttr.setUsesArrayDataBuilder(true);
   addAttribute(inputShellComponents);

   MFnStringData defaultString;
   MObject defaultVal = defaultString.create("map1");
   uvSetNameAttr = tAttr.create("uvSetName", "uvSet", MFnData::kString, defaultVal);
   addAttribute(uvSetNameAttr);

   offsetAttr = nAttr.create("offset", "ofs", MFnNumericData::kFloat, 0.01f);
   nAttr.setMin(0.0f);
   addAttribute(offsetAttr);

   // Affect geometry
   attributeAffects(inputShellComponents, outputGeom);
   attributeAffects(uvSetNameAttr, outputGeom);
   attributeAffects(offsetAttr, outputGeom);

   return MS::kSuccess;
}


MStatus UVShellExpandNode::deform(MDataBlock& data,
   MItGeometry& iter,
   const MMatrix& /*localToWorldMatrix*/,
   unsigned int geomIndex)
{
   MStatus status;

   float env = data.inputValue(envelope).asFloat();
   if (env == 0.0f) return MS::kSuccess;

   MArrayDataHandle inputArray = data.outputArrayValue(input);
   inputArray.jumpToElement(geomIndex);
   MObject meshObj = inputArray.outputValue().child(inputGeom).asMesh();

   MFnMesh meshFn(meshObj, &status);
   CHECK_MSTATUS_AND_RETURN_IT(status);

   MString uvSet = data.inputValue(uvSetNameAttr).asString();
   float offset = data.inputValue(offsetAttr).asFloat();

   MFloatArray uArray, vArray;
   meshFn.getUVs(uArray, vArray, &uvSet);

   MArrayDataHandle shellArrayHandle = data.inputArrayValue(inputShellComponents);
   unsigned int shellCount = shellArrayHandle.elementCount();

   for (unsigned int i = 0; i < shellCount; ++i) {
       shellArrayHandle.jumpToElement(i);
       MObject compObj = shellArrayHandle.inputValue().data();
       MFnComponentListData compList(compObj);

       std::unordered_set<int> seenUvIds;

       for (unsigned int j = 0; j < compList.length(); ++j) {
           MObject comp = compList[j];
           if (comp.apiType() != MFn::kMeshPolygonComponent) continue;

           MFnSingleIndexedComponent singleComp(comp);
           MIntArray faceIds;
           singleComp.getElements(faceIds);

           for (unsigned int f = 0; f < faceIds.length(); ++f) {
               int faceId = faceIds[f];

               MItMeshPolygon faceIt(meshObj);
               int numVerts = faceIt.polygonVertexCount();

               for (int vi = 0; vi < numVerts; ++vi) {
                   int uvId = -1;
                   if (faceIt.getUVIndex(vi, uvId, &uvSet) == MS::kSuccess &&
                       seenUvIds.insert(uvId).second) {
                       uArray[uvId] += offset * 0.5f;
                       vArray[uvId] += offset * 0.5f;
                   }
               }
           }
       }
   }

   meshFn.setUVs(uArray, vArray, &uvSet);
   return MS::kSuccess;
}