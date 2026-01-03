#pragma once

#include <maya/MTypeId.h>
#include <maya/MObject.h>
#include <maya/MPxDeformerNode.h>


class UVShellExpandNode : public MPxDeformerNode {
public:
   UVShellExpandNode() = default;
   ~UVShellExpandNode() override = default;

   static void* creator();
   static MStatus initialize();

   MStatus deform(MDataBlock& data,
       MItGeometry& iter,
       const MMatrix& localToWorldMatrix,
       unsigned int geomIndex) override;


   static MTypeId id;

   // Attributes
   static MObject inputShellComponents;  // array of component lists
   static MObject uvSetNameAttr;
   static MObject offsetAttr;
};
