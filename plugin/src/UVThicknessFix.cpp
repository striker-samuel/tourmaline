#include "UVThicknessFix.h"
#include <maya/MFnPlugin.h>
#include <maya/MGlobal.h>
#include "UVScalerNode.h"
#include "UVZeroShellNode.h"
#include "UVShellExpandNode.h"
#include "ZeroUVShiftCmd.h"
#include "FlipNEgativeUvsCmd.h"


// Command implementation
void* UVThicknessFix::creator() {
    return new UVThicknessFix();
}

MStatus UVThicknessFix::doIt(const MArgList& args) {
    MGlobal::displayInfo("UVThicknessFix command executed!");
    return MS::kSuccess;
}


// Plugin registration
MStatus initializePlugin(MObject obj) {
    MFnPlugin plugin(obj, "Samuel Martin Canto samuel.martin1999@outlook.com", "1.0", "Any");

    plugin.registerNode("uvScalerNode", UVScalerNode::id, UVScalerNode::creator,
        UVScalerNode::initialize, MPxNode::kDependNode);

    plugin.registerNode("uvZeroAreaNode", UVZeroAreaNode::id, UVZeroAreaNode::creator,
        UVZeroAreaNode::initialize, MPxNode::kDependNode);

    plugin.registerCommand("zeroUVShift",
        ZeroUVShiftCmd::creator,
        ZeroUVShiftCmd::newSyntax
    );

    plugin.registerCommand("shiftNegativeUVs",
        ShiftNegativeUVsCmd::creator,
        ShiftNegativeUVsCmd::newSyntax
    );

    plugin.registerNode("uvShellExpandNode", UVShellExpandNode::id, UVShellExpandNode::creator,
        UVShellExpandNode::initialize, MPxNode::kDeformerNode);

 
    MStatus status = plugin.registerCommand("uvThicknessFix", UVThicknessFix::creator);
    if (!status) status.perror("registerCommand failed");
    return status;
}

MStatus uninitializePlugin(MObject obj) {
    MFnPlugin plugin(obj);

    plugin.deregisterCommand("zeroUVShift");
    plugin.deregisterCommand("shiftNegativeUVs");

    plugin.deregisterNode(UVScalerNode::id);
    plugin.deregisterNode(UVZeroAreaNode::id);
    plugin.deregisterNode(UVShellExpandNode::id);


    MStatus status = plugin.deregisterCommand("uvThicknessFix");
    if (!status) status.perror("deregisterCommand failed");
    return status;
}

