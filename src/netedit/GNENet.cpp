/****************************************************************************/
// Eclipse SUMO, Simulation of Urban MObility; see https://eclipse.org/sumo
// Copyright (C) 2001-2018 German Aerospace Center (DLR) and others.
// This program and the accompanying materials
// are made available under the terms of the Eclipse Public License v2.0
// which accompanies this distribution, and is available at
// http://www.eclipse.org/legal/epl-v20.html
// SPDX-License-Identifier: EPL-2.0
/****************************************************************************/
/// @file    GNENet.cpp
/// @author  Jakob Erdmann
/// @date    Feb 2011
/// @version $Id$
///
// A visual container for GNE-network-components such as GNEEdge and GNEJunction.
// GNE components wrap netbuild-components and supply visualisation and editing
// capabilities (adapted from GUINet)
//
// Workflow (rough draft)
//   use NILoader to fill
//   do netedit stuff
//   call compute to save results
//
/****************************************************************************/


// ===========================================================================
// included modules
// ===========================================================================
#ifdef _MSC_VER
#include <windows_config.h>
#else
#include <config.h>
#endif

#include <map>
#include <set>
#include <vector>

#include <netbuild/NBAlgorithms.h>
#include <netwrite/NWFrame.h>
#include <netwrite/NWWriter_XML.h>
#include <netwrite/NWWriter_SUMO.h>
#include <utility>
#include <utils/common/MsgHandler.h>
#include <utils/common/RGBColor.h>
#include <utils/common/StringUtils.h>
#include <utils/geom/GeomConvHelper.h>
#include <utils/gui/div/GUIParameterTableWindow.h>
#include <utils/gui/globjects/GUIGLObjectPopupMenu.h>
#include <utils/gui/globjects/GUIGlObjectStorage.h>
#include <utils/gui/globjects/GUIGlObject_AbstractAdd.h>
#include <utils/gui/images/GUITextureSubSys.h>
#include <utils/gui/windows/GUIMainWindow.h>
#include <utils/shapes/ShapeContainer.h>
#include <utils/xml/SUMOXMLDefinitions.h>
#include <utils/xml/XMLSubSys.h>
#include <netedit/changes/GNEChange_Additional.h>
#include <netedit/changes/GNEChange_Attribute.h>
#include <netedit/changes/GNEChange_Connection.h>
#include <netedit/changes/GNEChange_Crossing.h>
#include <netedit/changes/GNEChange_Edge.h>
#include <netedit/changes/GNEChange_Junction.h>
#include <netedit/changes/GNEChange_Lane.h>
#include <netedit/changes/GNEChange_Selection.h>
#include <netedit/changes/GNEChange_Shape.h>
#include <netedit/frames/GNEInspectorFrame.h>
#include <netedit/frames/GNEAdditionalFrame.h>
#include <netedit/dialogs/GNEDialog_FixAdditionalPositions.h>
#include <netedit/netelements/GNEEdge.h>
#include <netedit/netelements/GNEJunction.h>
#include <netedit/netelements/GNELane.h>
#include <netedit/netelements/GNEConnection.h>
#include <netedit/netelements/GNECrossing.h>
#include <netedit/additionals/GNEAdditional.h>
#include <netedit/additionals/GNEAdditionalHandler.h>
#include <netedit/additionals/GNECalibratorRoute.h>
#include <netedit/additionals/GNECalibratorVehicleType.h>
#include <netedit/additionals/GNEDetector.h>
#include <netedit/additionals/GNEDetectorE2.h>
#include <netedit/additionals/GNEPOI.h>
#include <netedit/additionals/GNEPoly.h>
#include <netedit/additionals/GNERerouter.h>
#include <netedit/additionals/GNERerouterInterval.h>
#include <netedit/additionals/GNEStoppingPlace.h>

#include "GNENet.h"
#include "GNEApplicationWindow.h"
#include "GNEUndoList.h"
#include "GNEViewNet.h"
#include "GNEViewParent.h"


// ===========================================================================
// FOX callback mapping
// ===========================================================================

FXIMPLEMENT_ABSTRACT(GNENet::GNEChange_ReplaceEdgeInTLS, GNEChange, NULL, 0)

// ===========================================================================
// static members
// ===========================================================================

const RGBColor GNENet::selectionColor(0, 0, 204, 255);
const RGBColor GNENet::selectedLaneColor(0, 0, 128, 255);
const RGBColor GNENet::selectedConnectionColor(0, 0, 100, 255);
const RGBColor GNENet::selectedAdditionalColor(0, 0, 150, 255);
const double GNENet::Z_INITIALIZED = 1;

// ===========================================================================
// member method definitions
// ===========================================================================

GNENet::GNENet(NBNetBuilder* netBuilder) :
    GUIGlObject(GLO_NETWORK, ""),
    ShapeContainer(),
    myViewNet(0),
    myNetBuilder(netBuilder),
    myJunctions(),
    myEdges(),
    myEdgeIDSupplier("gneE", netBuilder->getEdgeCont().getAllNames()),
    myJunctionIDSupplier("gneJ", netBuilder->getNodeCont().getAllNames()),
    myNeedRecompute(true),
    myAdditionalsSaved(true),
    myShapesSaved(true),
    myTLSProgramsSaved(true) {
    // set net in gIDStorage
    GUIGlObjectStorage::gIDStorage.setNetObject(this);

    // init junction and edges
    initJunctionsAndEdges();

    // check Z boundary
    if (myZBoundary.ymin() != Z_INITIALIZED) {
        myZBoundary.add(0, 0);
    }
}


GNENet::~GNENet() {
    // Drop Edges
    for (auto it : myEdges) {
        it.second->decRef("GNENet::~GNENet");
        // show extra information for tests
        if (OptionsCont::getOptions().getBool("gui-testing-debug")) {
            WRITE_WARNING("Deleting unreferenced " + toString(it.second->getTag()) + " '" + it.second->getID() + "' in GNENet destructor");
        }
        delete it.second;
    }
    // Drop junctions
    for (auto it : myJunctions) {
        it.second->decRef("GNENet::~GNENet");
        // show extra information for tests
        if (OptionsCont::getOptions().getBool("gui-testing-debug")) {
            WRITE_WARNING("Deleting unreferenced " + toString(it.second->getTag()) + " '" + it.second->getID() + "' in GNENet destructor");
        }
        delete it.second;
    }
    // Drop Additionals (Only used for additionals that were inserted without using GNEChange_Additional)
    for (auto it : myAdditionals) {
        // decrease reference manually (because it was increased manually in GNEAdditionalHandler)
        it.second->decRef();
        // show extra information for tests
        if (OptionsCont::getOptions().getBool("gui-testing-debug")) {
            WRITE_WARNING("Deleting unreferenced " + toString(it.second->getTag()) + " '" + it.second->getID() + "' in GNENet destructor");
        }
        delete it.second;
    }
    // Drop calibrator routes (Only used for additionals that were inserted without using GNEChange_CalibratorItem)
    for (auto it : myCalibratorRoutes) {
        // decrease reference manually (because it was increased manually in GNEAdditionalHandler)
        it.second->decRef();
        // show extra information for tests
        if (OptionsCont::getOptions().getBool("gui-testing-debug")) {
            WRITE_WARNING("Deleting unreferenced " + toString(it.second->getTag()) + " '" + it.second->getID() + "' in GNENet destructor");
        }
        delete it.second;
    }
    // Drop calibrator vehicle types (Only used for additionals that were inserted without using GNEChange_CalibratorItem)
    for (auto it : myCalibratorVehicleTypes) {
        // decrease reference manually (because it was increases manually in GNEAdditionalHandler)
        it.second->decRef();
        // show extra information for tests
        if (OptionsCont::getOptions().getBool("gui-testing-debug")) {
            WRITE_WARNING("Deleting unreferenced " + toString(it.second->getTag()) + " '" + it.second->getID() + "' in GNENet destructor");
        }
        delete it.second;
    }
    // show extra information for tests
    if (OptionsCont::getOptions().getBool("gui-testing-debug")) {
        WRITE_WARNING("Deleting net builder in GNENet destructor");
    }
    delete myNetBuilder;
}


const Boundary&
GNENet::getBoundary() const {
    // SUMORTree is also a Boundary
    return myGrid;
}


GUIGLObjectPopupMenu*
GNENet::getPopUpMenu(GUIMainWindow& app, GUISUMOAbstractView& parent) {
    GUIGLObjectPopupMenu* ret = new GUIGLObjectPopupMenu(app, parent, *this);
    buildPopupHeader(ret, app);
    buildCenterPopupEntry(ret);
    buildPositionCopyEntry(ret, false);
    return ret;
}


GUIParameterTableWindow*
GNENet::getParameterWindow(GUIMainWindow& app, GUISUMOAbstractView&) {
    // Nets lanes don't have attributes
    GUIParameterTableWindow* ret = new GUIParameterTableWindow(app, *this, 2);
    // close building
    ret->closeBuilding();
    return ret;
}


void
GNENet::drawGL(const GUIVisualizationSettings& /*s*/) const {
}


bool
GNENet::addPolygon(const std::string& id, const std::string& type, const RGBColor& color, double layer, double angle,
                   const std::string& imgFile, bool relativePath, const PositionVector& shape, bool geo, bool fill, bool /*ignorePruning*/) {
    // check if ID is duplicated
    if (myPolygons.get(id) == NULL) {
        // create poly
        myViewNet->getUndoList()->p_begin("add " + toString(SUMO_TAG_POLY));
        GNEPoly* poly = new GNEPoly(this, id, type, shape, geo, fill, color, layer, angle, imgFile, relativePath, false, false);
        myViewNet->getUndoList()->add(new GNEChange_Shape(poly, true), true);
        myViewNet->getUndoList()->p_end();
        return true;
    } else {
        return false;
    }
}


bool
GNENet::addPOI(const std::string& id, const std::string& type, const RGBColor& color, const Position& pos, bool geo,
               const std::string& lane, double posOverLane, double posLat, double layer, double angle,
               const std::string& imgFile, bool relativePath, double width, double height, bool /*ignorePruning*/) {
    // check if ID is duplicated
    if (myPOIs.get(id) == NULL) {
        // create POI or POILane depending of parameter lane
        if (lane == "") {
            // create POI
            GNEPOI* poi = new GNEPOI(this, id, type, color, pos, geo, layer, angle, imgFile, relativePath, width, height, false);
            if (myPOIs.add(poi->getID(), poi)) {
                myViewNet->getUndoList()->p_begin("add " + toString(poi->getTag()));
                myViewNet->getUndoList()->add(new GNEChange_Shape(poi, true), true);
                myViewNet->getUndoList()->p_end();
                return true;
            } else {
                throw ProcessError("Error adding GNEPOI into shapeContainer");
            }
        } else {
            // create POI over lane
            GNELane* retrievedLane = retrieveLane(lane);
            GNEPOI* poi = new GNEPOI(this, id, type, color, layer, angle, imgFile, relativePath, retrievedLane, posOverLane, posLat, width, height, false);
            if (myPOIs.add(poi->getID(), poi)) {
                myViewNet->getUndoList()->p_begin("add " + toString(poi->getTag()));
                myViewNet->getUndoList()->add(new GNEChange_Shape(poi, true), true);
                myViewNet->getUndoList()->p_end();
                return true;
            } else {
                throw ProcessError("Error adding GNEPOI over lane into shapeContainer");
            }
        }
    } else {
        return false;
    }
}


Boundary
GNENet::getCenteringBoundary() const {
    return getBoundary();
}


const Boundary&
GNENet::getZBoundary() const {
    return myZBoundary;
}


SUMORTree&
GNENet::getVisualisationSpeedUp() {
    return myGrid;
}


const SUMORTree&
GNENet::getVisualisationSpeedUp() const {
    return myGrid;
}


GNEJunction*
GNENet::createJunction(const Position& pos, GNEUndoList* undoList) {
    std::string id = myJunctionIDSupplier.getNext();
    NBNode* nbn = new NBNode(id, pos);
    GNEJunction* junction = new GNEJunction(*nbn, this);
    undoList->add(new GNEChange_Junction(junction, true), true);
    assert(myJunctions[id]);
    return junction;
}


GNEEdge*
GNENet::createEdge(
    GNEJunction* src, GNEJunction* dest, GNEEdge* tpl, GNEUndoList* undoList,
    const std::string& suggestedName,
    bool wasSplit,
    bool allowDuplicateGeom,
    bool recomputeConnections) {
    // prevent duplicate edge (same geometry)
    const EdgeVector& outgoing = src->getNBNode()->getOutgoingEdges();
    for (EdgeVector::const_iterator it = outgoing.begin(); it != outgoing.end(); it++) {
        if ((*it)->getToNode() == dest->getNBNode() && (*it)->getGeometry().size() == 2) {
            if (!allowDuplicateGeom) {
                return 0;
            }
        }
    }

    std::string id;
    if (suggestedName != "" && !retrieveEdge(suggestedName, false)) {
        id = suggestedName;
        reserveEdgeID(id);
    } else {
        id = myEdgeIDSupplier.getNext();
    }

    GNEEdge* edge;
    if (tpl) {
        NBEdge* nbeTpl = tpl->getNBEdge();
        NBEdge* nbe = new NBEdge(id, src->getNBNode(), dest->getNBNode(), nbeTpl);
        edge = new GNEEdge(*nbe, this, wasSplit);
    } else {
        // default if no template is given
        double defaultSpeed = 50 / 3.6;
        std::string defaultType = "";
        int defaultNrLanes = 1;
        int defaultPriority = 1;
        double defaultWidth = NBEdge::UNSPECIFIED_WIDTH;
        double defaultOffset = NBEdge::UNSPECIFIED_OFFSET;
        NBEdge* nbe = new NBEdge(id, src->getNBNode(), dest->getNBNode(),
                                 defaultType, defaultSpeed,
                                 defaultNrLanes, defaultPriority,
                                 defaultWidth,
                                 defaultOffset);
        edge = new GNEEdge(*nbe, this, wasSplit);
    }
    undoList->p_begin("create " + toString(SUMO_TAG_EDGE));
    undoList->add(new GNEChange_Edge(edge, true), true);
    if (recomputeConnections) {
        src->setLogicValid(false, undoList);
        dest->setLogicValid(false, undoList);
    }
    requireRecompute();
    undoList->p_end();
    assert(myEdges[id]);
    return edge;
}


void
GNENet::deleteJunction(GNEJunction* junction, GNEUndoList* undoList) {
    // we have to delete all incident edges because they cannot exist without that junction
    // all deletions must be undone/redone together so we start a new command group
    // @todo if any of those edges are dead-ends should we remove their orphan junctions as well?
    undoList->p_begin("delete " + toString(SUMO_TAG_JUNCTION));

    // delete all crossings vinculated with junction
    while (junction->getGNECrossings().size() > 0) {
        deleteCrossing(junction->getGNECrossings().front(), undoList);
    }

    // find all crossings of neightbour junctions that shares an edge of this junction
    std::vector<GNECrossing*> crossingsToRemove;
    std::vector<GNEJunction*> junctionNeighbours = junction->getJunctionNeighbours();
    for (auto i : junctionNeighbours) {
        // iterate over crossing of neighbour juntion
        for (auto j : i->getGNECrossings()) {
            // if at least one of the edges of junction to remove belongs to a crossing of the neighbour junction, delete it
            if (j->checkEdgeBelong(junction->getGNEEdges())) {
                crossingsToRemove.push_back(j);
            }
        }
    }

    // delete crossings top remove
    for (auto i : crossingsToRemove) {
        deleteCrossing(i, undoList);
    }

    // deleting edges changes in the underlying EdgeVector so we have to make a copy
    const EdgeVector incident = junction->getNBNode()->getEdges();
    for (EdgeVector::const_iterator it = incident.begin(); it != incident.end(); it++) {
        deleteEdge(myEdges[(*it)->getID()], undoList, true);
    }

    // remove any traffic lights from the traffic light container (avoids lots of warnings)
    junction->setAttribute(SUMO_ATTR_TYPE, toString(NODETYPE_PRIORITY), undoList);

    // delete edge
    undoList->add(new GNEChange_Junction(junction, false), true);
    undoList->p_end();
}


void
GNENet::deleteEdge(GNEEdge* edge, GNEUndoList* undoList, bool recomputeConnections) {
    undoList->p_begin("delete " + toString(SUMO_TAG_EDGE));
    // remove edge of additional parents (For example, Rerouters)
    edge->removeEdgeOfAdditionalParents(undoList, false);
    // delete all additionals childs of edge
    while (edge->getAdditionalChilds().size() > 0) {
        myViewNet->getViewParent()->getAdditionalFrame()->removeAdditional(edge->getAdditionalChilds().front());
    }
    // delete all additionals childs of lane
    for (auto i : edge->getLanes()) {
        // remove lane of additional parents (For example, VSS)
        i->removeLaneOfAdditionalParents(undoList, false);
        while (i->getAdditionalChilds().size() > 0) {
            myViewNet->getViewParent()->getAdditionalFrame()->removeAdditional(i->getAdditionalChilds().front());
        }
    }
    // delete shapes childs of lane
    for (auto i : edge->getLanes()) {
        std::vector<GNEShape*> copyOfLaneShapes = i->getShapeChilds();
        for (auto j : copyOfLaneShapes) {
            undoList->add(new GNEChange_Shape(j, false), true);
        }
    }
    // remove edge from crossings related with this edge
    edge->getGNEJunctionSource()->removeEdgeFromCrossings(edge, undoList);
    edge->getGNEJunctionDestiny()->removeEdgeFromCrossings(edge, undoList);
    // update affected connections
    if (recomputeConnections) {
        edge->getGNEJunctionSource()->setLogicValid(false, undoList);
        edge->getGNEJunctionDestiny()->setLogicValid(false, undoList);
    } else {
        edge->getGNEJunctionSource()->removeConnectionsTo(edge, undoList, true);
        edge->getGNEJunctionSource()->removeConnectionsFrom(edge, undoList, true);
    }
    // if junction source is a TLS and after deletion will have only an edge, remove TLS
    if(edge->getGNEJunctionSource()->getNBNode()->isTLControlled() && (edge->getGNEJunctionSource()->getGNEOutgoingEdges().size() <= 1)) {
            edge->getGNEJunctionSource()->setAttribute(SUMO_ATTR_TYPE, toString(NODETYPE_PRIORITY), undoList);
    }
    // if junction destiny is a TLS and after deletion will have only an edge, remove TLS
    if(edge->getGNEJunctionDestiny()->getNBNode()->isTLControlled() && (edge->getGNEJunctionDestiny()->getGNEIncomingEdges().size() <= 1)) {
            edge->getGNEJunctionDestiny()->setAttribute(SUMO_ATTR_TYPE, toString(NODETYPE_PRIORITY), undoList);
    }
    // Delete edge
    undoList->add(new GNEChange_Edge(edge, false), true);
    // remove edge requieres always a recompute (due geometry and connections)
    requireRecompute();
    // finish delete edge
    undoList->p_end();
}


void
GNENet::replaceIncomingEdge(GNEEdge* which, GNEEdge* by, GNEUndoList* undoList) {
    undoList->p_begin("replace " + toString(SUMO_TAG_EDGE));
    undoList->p_add(new GNEChange_Attribute(by, SUMO_ATTR_TO, which->getAttribute(SUMO_ATTR_TO)));
    // replace in additionals childs of edge
    while (which->getAdditionalChilds().size() > 0) {
        undoList->p_add(new GNEChange_Attribute(which->getAdditionalChilds().front(), SUMO_ATTR_EDGE, by->getID()));
    }
    // replace in additionals childs of lane
    for (auto i : which->getLanes()) {
        std::vector<GNEAdditional*> copyOfLaneAdditionals = i->getAdditionalChilds();
        for (auto j : copyOfLaneAdditionals) {
            UNUSED_PARAMETER(j);
            undoList->p_add(new GNEChange_Attribute(i, SUMO_ATTR_LANE, by->getNBEdge()->getLaneID(i->getIndex())));
        }
    }
    // replace in shapes childs of lane
    for (auto i : which->getLanes()) {
        std::vector<GNEShape*> copyOfLaneShapes = i->getShapeChilds();
        for (auto j : copyOfLaneShapes) {
            UNUSED_PARAMETER(j);
            undoList->p_add(new GNEChange_Attribute(i, SUMO_ATTR_LANE, by->getNBEdge()->getLaneID(i->getIndex())));
        }
    }
    // replace in rerouters
    for (auto rerouter : which->getAdditionalParents()) {
        replaceInListAttribute(rerouter, SUMO_ATTR_EDGES, which->getID(), by->getID(), undoList);
    }
    // replace in crossings
    for (auto crossing : which->getGNEJunctionDestiny()->getGNECrossings()) {
        // if at least one of the edges of junction to remove belongs to a crossing of the source junction, delete it
        replaceInListAttribute(crossing, SUMO_ATTR_EDGES, which->getID(), by->getID(), undoList);
    }
    // fix connections (make a copy because they will be modified
    std::vector<NBEdge::Connection> connections = which->getNBEdge()->getConnections();
    for (auto con : connections) {
        undoList->add(new GNEChange_Connection(which, con, false, false), true);
        undoList->add(new GNEChange_Connection(by, con, false, true), true);
    }
    undoList->add(new GNEChange_ReplaceEdgeInTLS(getTLLogicCont(), which->getNBEdge(), by->getNBEdge()), true);
    // Delete edge
    undoList->add(new GNEChange_Edge(which, false), true);
    // finish replace edge
    undoList->p_end();
}


void
GNENet::deleteLane(GNELane* lane, GNEUndoList* undoList, bool recomputeConnections) {
    GNEEdge* edge = &lane->getParentEdge();
    if (edge->getNBEdge()->getNumLanes() == 1) {
        // remove the whole edge instead
        deleteEdge(edge, undoList, recomputeConnections);
    } else {
        undoList->p_begin("delete " + toString(SUMO_TAG_LANE));
        // remove lane of additional parents (For example, VSS)
        lane->removeLaneOfAdditionalParents(undoList, false);
        // delete additionals childs of lane
        while (lane->getAdditionalChilds().size() > 0) {
            myViewNet->getViewParent()->getAdditionalFrame()->removeAdditional(lane->getAdditionalChilds().front());
        }
        // delete POIShapes of Lane
        while (lane->getShapeChilds().size() > 0) {
            undoList->add(new GNEChange_Shape(lane->getShapeChilds().front(), false), true);
        }
        // update affected connections
        if (recomputeConnections) {
            edge->getGNEJunctionSource()->setLogicValid(false, undoList);
            edge->getGNEJunctionDestiny()->setLogicValid(false, undoList);
        } else {
            edge->getGNEJunctionSource()->removeConnectionsTo(edge, undoList, true, lane->getIndex());
            edge->getGNEJunctionSource()->removeConnectionsFrom(edge, undoList, true, lane->getIndex());
        }
        // delete lane
        const NBEdge::Lane& laneAttrs = edge->getNBEdge()->getLaneStruct(lane->getIndex());
        undoList->add(new GNEChange_Lane(edge, lane, laneAttrs, false, recomputeConnections), true);
        // remove lane requieres always a recompute (due geometry and connections)
        requireRecompute();
        undoList->p_end();
    }
}


void
GNENet::deleteConnection(GNEConnection* connection, GNEUndoList* undoList) {
    undoList->p_begin("delete " + toString(SUMO_TAG_CONNECTION));
    // obtain NBConnection to remove
    NBConnection deleted = connection->getNBConnection();
    GNEJunction* junctionDestiny = connection->getEdgeFrom()->getGNEJunctionDestiny();
    junctionDestiny->markAsModified(undoList);
    undoList->add(new GNEChange_Connection(connection->getEdgeFrom(), connection->getNBEdgeConnection(), connection->isnetElementSelected(), false), true);
    junctionDestiny->invalidateTLS(undoList, deleted);
    // remove connection requieres always a recompute (due geometry and connections)
    requireRecompute();
    undoList->p_end();
}


void
GNENet::deleteCrossing(GNECrossing* crossing, GNEUndoList* undoList) {
    undoList->p_begin("delete crossing");
    // remove it using GNEChange_Crossing
    undoList->add(new GNEChange_Crossing(crossing->getParentJunction(), crossing->getNBCrossing()->edges,
                                         crossing->getNBCrossing()->width, crossing->getNBCrossing()->priority,
                                         crossing->getNBCrossing()->customTLIndex,
                                         crossing->getNBCrossing()->customTLIndex2,
                                         crossing->getNBCrossing()->customShape, false), true);
    // remove crossing requieres always a recompute (due geometry and connections)
    requireRecompute();
    undoList->p_end();
}


void
GNENet::deleteShape(GNEShape* shape, GNEUndoList* undoList) {
    undoList->p_begin("delete " + toString(shape->getTag()));
    // delete shape
    undoList->add(new GNEChange_Shape(shape, false), true);
    undoList->p_end();
}


void
GNENet::duplicateLane(GNELane* lane, GNEUndoList* undoList, bool recomputeConnections) {
    undoList->p_begin("duplicate " + toString(SUMO_TAG_LANE));
    GNEEdge* edge = &lane->getParentEdge();
    const NBEdge::Lane& laneAttrs = edge->getNBEdge()->getLaneStruct(lane->getIndex());
    GNELane* newLane = new GNELane(*edge, lane->getIndex());
    undoList->add(new GNEChange_Lane(edge, newLane, laneAttrs, true, recomputeConnections), true);
    requireRecompute();
    undoList->p_end();
}


bool
GNENet::restrictLane(SUMOVehicleClass vclass, GNELane* lane, GNEUndoList* undoList) {
    bool addRestriction = true;
    if (vclass == SVC_PEDESTRIAN) {
        GNEEdge& edge = lane->getParentEdge();
        for (auto i : edge.getLanes()) {
            if (i->isRestricted(SVC_PEDESTRIAN)) {
                // prevent adding a 2nd sidewalk
                addRestriction = false;
            } else {
                // ensure that the sidewalk is used exclusively
                const SVCPermissions allOldWithoutPeds = edge.getNBEdge()->getPermissions(i->getIndex()) & ~SVC_PEDESTRIAN;
                i->setAttribute(SUMO_ATTR_ALLOW, getVehicleClassNames(allOldWithoutPeds), undoList);
            }
        }
    }
    // restrict the lane
    if (addRestriction) {
        lane->setAttribute(SUMO_ATTR_ALLOW, toString(vclass), undoList);
        lane->setAttribute(SUMO_ATTR_WIDTH, toString(OptionsCont::getOptions().getFloat("default.sidewalk-width")), undoList);
        return true;
    } else {
        return false;
    }
}


bool
GNENet::addSRestrictedLane(SUMOVehicleClass vclass, GNEEdge& edge, GNEUndoList* undoList) {
    // First check that edge don't have a sidewalk
    for (auto i : edge.getLanes()) {
        if (i->isRestricted(vclass)) {
            return false;
        }
    }
    // duplicate last lane
    duplicateLane(edge.getLanes().at(0), undoList, true);
    // transform the created (last) lane to a sidewalk
    return restrictLane(vclass, edge.getLanes()[0], undoList);
}


bool
GNENet::removeRestrictedLane(SUMOVehicleClass vclass, GNEEdge& edge, GNEUndoList* undoList) {
    // iterate over lanes of edge
    for (auto i : edge.getLanes()) {
        if (i->isRestricted(vclass)) {
            // Delete lane
            deleteLane(i, undoList, true);
            return true;
        }
    }
    return false;
}


GNEJunction*
GNENet::splitEdge(GNEEdge* edge, const Position& pos, GNEUndoList* undoList, GNEJunction* newJunction) {
    undoList->p_begin("split " + toString(SUMO_TAG_EDGE));
    if (newJunction == 0) {
        newJunction = createJunction(pos, undoList);
    }
    // compute geometry
    const PositionVector& oldGeom = edge->getNBEdge()->getGeometry();
    const double linePos = oldGeom.nearest_offset_to_point2D(pos, false);
    std::pair<PositionVector, PositionVector> newGeoms = oldGeom.splitAt(linePos);
    const std::string shapeEnd = edge->getAttribute(GNE_ATTR_SHAPE_END);
    // figure out the new name
    int posBase = 0;
    std::string baseName = edge->getMicrosimID();
    if (edge->wasSplit()) {
        const std::string::size_type sep_index = baseName.rfind('.');
        if (sep_index != std::string::npos) { // edge may have been renamed in between
            std::string posString = baseName.substr(sep_index + 1);
            try {
                posBase = GNEAttributeCarrier::parse<int>(posString.c_str());
                baseName = baseName.substr(0, sep_index); // includes the .
            } catch (NumberFormatException) {
            }
        }
    }
    baseName += '.';
    // create a new edge from the new junction to the previous destination
    GNEEdge* secondPart = createEdge(newJunction, edge->getGNEJunctionDestiny(), edge,
                                     undoList, baseName + toString(posBase + (int)linePos), true, false, false);
    // fix connections from the split edge (must happen before changing SUMO_ATTR_TO)
    edge->getGNEJunctionDestiny()->replaceIncomingConnections(edge, secondPart, undoList);
    // modify the edge so that it ends at the new junction (and all incoming connections are preserved
    undoList->p_add(new GNEChange_Attribute(edge, SUMO_ATTR_TO, newJunction->getID()));
    // fix first part of geometry
    newGeoms.first.pop_back();
    newGeoms.first.erase(newGeoms.first.begin());
    edge->setAttribute(GNE_ATTR_SHAPE_END, "", undoList);
    edge->setAttribute(SUMO_ATTR_SHAPE, toString(newGeoms.first), undoList);
    // fix second part of geometry
    secondPart->setAttribute(GNE_ATTR_SHAPE_END, shapeEnd, undoList);
    newGeoms.second.pop_back();
    newGeoms.second.erase(newGeoms.second.begin());
    secondPart->setAttribute(SUMO_ATTR_SHAPE, toString(newGeoms.second), undoList);
    // reconnect across the split
    for (int i = 0; i < (int)edge->getLanes().size(); ++i) {
        undoList->add(new GNEChange_Connection(edge, NBEdge::Connection(i, secondPart->getNBEdge(), i), false, true), true);
    }
    undoList->p_end();
    return newJunction;
}


void
GNENet::splitEdgesBidi(const std::set<GNEEdge*>& edges, const Position& pos, GNEUndoList* undoList) {
    GNEJunction* newJunction = 0;
    undoList->p_begin("split " + toString(SUMO_TAG_EDGE) + "s");
    for (auto it : edges) {
        newJunction = splitEdge(it, pos, undoList, newJunction);
    }
    undoList->p_end();
}


void
GNENet::reverseEdge(GNEEdge* edge, GNEUndoList* undoList) {
    undoList->p_begin("reverse " + toString(SUMO_TAG_EDGE));
    deleteEdge(edge, undoList, false); // still exists. we delete it so we can reuse the name in case of resplit
    GNEEdge* reversed = createEdge(edge->getGNEJunctionDestiny(), edge->getGNEJunctionSource(), edge, undoList, edge->getID(), false, true);
    assert(reversed != 0);
    reversed->setAttribute(SUMO_ATTR_SHAPE, toString(edge->getNBEdge()->getInnerGeometry().reverse()), undoList);
    undoList->p_end();
}


GNEEdge*
GNENet::addReversedEdge(GNEEdge* edge, GNEUndoList* undoList) {
    undoList->p_begin("add reversed " + toString(SUMO_TAG_EDGE));
    GNEEdge* reversed = NULL;
    if (edge->getNBEdge()->getLaneSpreadFunction() == LANESPREAD_RIGHT || isRailway(edge->getNBEdge()->getPermissions())) {
        // for rail edges, we assume bi-directional tracks are wanted
        reversed = createEdge(edge->getGNEJunctionDestiny(), edge->getGNEJunctionSource(), edge, undoList, "-" + edge->getID(), false, true);
        assert(reversed != 0);
        reversed->setAttribute(SUMO_ATTR_SHAPE, toString(edge->getNBEdge()->getInnerGeometry().reverse()), undoList);
    } else {
        // if the edge is centered it should probably connect somewhere else
        // make it easy to move and reconnect it
        PositionVector orig = edge->getNBEdge()->getGeometry();
        PositionVector origInner = edge->getNBEdge()->getInnerGeometry();
        const double tentativeShift = edge->getNBEdge()->getTotalWidth() + 2;
        orig.move2side(-tentativeShift);
        origInner.move2side(-tentativeShift);
        GNEJunction* src = createJunction(orig.back(), undoList);
        GNEJunction* dest = createJunction(orig.front(), undoList);
        reversed = createEdge(src, dest, edge, undoList, "-" + edge->getID(), false, true);
        assert(reversed != 0);
        reversed->setAttribute(SUMO_ATTR_SHAPE, toString(origInner.reverse()), undoList);
        reversed->setAttribute(SUMO_ATTR_SHAPE, toString(origInner.reverse()), undoList);
        // select the new edge and its nodes
        reversed->setAttribute(GNE_ATTR_SELECTED, "true", undoList);
        src->setAttribute(GNE_ATTR_SELECTED, "true", undoList);
        dest->setAttribute(GNE_ATTR_SELECTED, "true", undoList);
    }
    undoList->p_end();
    return reversed;
}


void
GNENet::mergeJunctions(GNEJunction* moved, GNEJunction* target, GNEUndoList* undoList) {
    undoList->p_begin("merge " + toString(SUMO_TAG_JUNCTION) + "s");
    // place moved junction in the same position of target junction
    moved->setAttribute(SUMO_ATTR_POSITION, target->getAttribute(SUMO_ATTR_POSITION), undoList);
    // deleting edges changes in the underlying EdgeVector so we have to make a copy
    const EdgeVector incoming = moved->getNBNode()->getIncomingEdges();
    for (EdgeVector::const_iterator it = incoming.begin(); it != incoming.end(); it++) {
        GNEEdge* oldEdge = myEdges[(*it)->getID()];
        remapEdge(oldEdge, oldEdge->getGNEJunctionSource(), target, undoList, true, false);
    }
    // deleting edges changes in the underlying EdgeVector so we have to make a copy
    const EdgeVector outgoing = moved->getNBNode()->getOutgoingEdges();
    for (EdgeVector::const_iterator it = outgoing.begin(); it != outgoing.end(); it++) {
        GNEEdge* oldEdge = myEdges[(*it)->getID()];
        remapEdge(oldEdge, target, oldEdge->getGNEJunctionDestiny(), undoList, false, true);
    }
    // deleted moved junction
    deleteJunction(moved, undoList);
    undoList->p_end();
}


void
GNENet::remapEdge(GNEEdge* oldEdge, GNEJunction* from, GNEJunction* to, GNEUndoList* undoList, bool preserveShapeStart, bool preserveShapeEnd) {
    // remove all crossings asociated to this edge before remap
    std::vector<GNECrossing*> crossingsOfOldEdge = oldEdge->getGNECrossings();
    for (auto i : crossingsOfOldEdge) {
        deleteCrossing(i, undoList);
    }
    // delete first so we can reuse the name, reference stays valid
    deleteEdge(oldEdge, undoList, false);
    if (from != to) {
        GNEEdge* newEdge = createEdge(from, to, oldEdge, undoList, oldEdge->getMicrosimID(), false, true);
        newEdge->setAttribute(SUMO_ATTR_SHAPE, oldEdge->getAttribute(SUMO_ATTR_SHAPE), undoList);
        // selectively preserve endpoints
        if (preserveShapeStart) {
            newEdge->setAttribute(GNE_ATTR_SHAPE_START, toString(oldEdge->getNBEdge()->getGeometry().front()), undoList);
        }
        if (preserveShapeEnd) {
            newEdge->setAttribute(GNE_ATTR_SHAPE_END, toString(oldEdge->getNBEdge()->getGeometry().back()), undoList);
        }
    }
    // @todo remap connectivity as well
}


bool
GNENet::checkJunctionPosition(const Position& pos) {
    // Check that there isn't another junction in the same position as Pos
    for (auto i : myJunctions) {
        if (i.second->getPositionInView() == pos) {
            return false;
        }
    }
    return true;
}


void
GNENet::save(OptionsCont& oc) {
    // compute without volatile options and update network
    computeAndUpdate(oc, false);
    // write network
    NWFrame::writeNetwork(oc, *myNetBuilder);
}


void
GNENet::savePlain(OptionsCont& oc) {
    // compute without volatile options
    computeAndUpdate(oc, false);
    NWWriter_XML::writeNetwork(oc, *myNetBuilder);
}


void
GNENet::saveJoined(OptionsCont& oc) {
    // compute without volatile options
    computeAndUpdate(oc, false);
    NWWriter_XML::writeJoinedJunctions(oc, myNetBuilder->getNodeCont());
}


void
GNENet::setViewNet(GNEViewNet* viewNet) {
    myViewNet = viewNet;
}


GNEJunction*
GNENet::retrieveJunction(const std::string& id, bool failHard) {
    if (myJunctions.count(id)) {
        return myJunctions[id];
    } else if (failHard) {
        // If junction wasn't found, throw exception
        throw UnknownElement("Junction " + id);
    } else {
        return NULL;
    }
}


GNEEdge*
GNENet::retrieveEdge(const std::string& id, bool failHard) {
    auto i = myEdges.find(id);
    // If edge was found
    if (i != myEdges.end()) {
        return i->second;
    } else if (failHard) {
        // If edge wasn't found, throw exception
        throw UnknownElement("Edge " + id);
    } else {
        return NULL;
    }
}


GNEEdge*
GNENet::retrieveEdge(GNEJunction* from, GNEJunction* to, bool failHard) {
    assert((from != NULL) && (to != NULL));
    // iterate over Junctions of net
    for (auto i : myEdges) {
        if ((i.second->getGNEJunctionSource() == from) && (i.second->getGNEJunctionDestiny() == to)) {
            return i.second;
        }
    }
    // if edge wasn' found, throw exception or return NULL
    if (failHard) {
        throw UnknownElement("Edge with from='" + from->getID() + "' and to='" + to->getID() + "'");
    } else {
        return NULL;
    }
}


GNEPoly*
GNENet::retrievePolygon(const std::string& id, bool failHard) const {
    if (myPolygons.get(id) != 0) {
        return reinterpret_cast<GNEPoly*>(myPolygons.get(id));
    } else if (failHard) {
        // If Polygon wasn't found, throw exception
        throw UnknownElement("Polygon " + id);
    } else {
        return NULL;
    }
}


GNEPOI*
GNENet::retrievePOI(const std::string& id, bool failHard) const {
    if (myPOIs.get(id) != 0) {
        return reinterpret_cast<GNEPOI*>(myPOIs.get(id));
    } else if (failHard) {
        // If POI wasn't found, throw exception
        throw UnknownElement("POI " + id);
    } else {
        return NULL;
    }
}


GNEConnection* 
GNENet::retrieveConnection(const std::string& id, bool failHard) const {
    // iterate over junctions
    for (auto i : myJunctions) {
        // iterate over connections
        for (auto j : i.second->getGNEConnections()) {
            if(j->getID() == id) {
                return j;
            }
        }
    }
    if (failHard) {
        // If POI wasn't found, throw exception
        throw UnknownElement("Connection " + id);
    } else {
        return NULL;
    }
}


GNECrossing* 
GNENet::retrieveCrossing(const std::string& id, bool failHard) const {
    // iterate over junctions
    for (auto i : myJunctions) {
        // iterate over crossings
        for (auto j : i.second->getGNECrossings()) {
            if (j->getID() == id) {
                return j;
            }
        }
    }
    if (failHard) {
        // If POI wasn't found, throw exception
        throw UnknownElement("Crossing " + id);
    } else {
        return NULL;
    }
}


std::vector<GNEEdge*>
GNENet::retrieveEdges(bool onlySelected) {
    std::vector<GNEEdge*> result;
    if(onlySelected) {
        // only returns selected edges
        for (auto i : mySelectedAttributeCarriers[GLO_EDGE]) {
            result.push_back(dynamic_cast<GNEEdge*>(i));
        }
    } else {
        // returns all edges
        for (auto i : myEdges) {
            result.push_back(i.second);
        }
    }
    return result;
}


std::vector<GNELane*>
GNENet::retrieveLanes(bool onlySelected) {
    std::vector<GNELane*> result;
    if(onlySelected) {
        // only returns selected edges
        for (auto i : mySelectedAttributeCarriers[GLO_LANE]) {
            result.push_back(dynamic_cast<GNELane*>(i));
        }
    } else {
        // returns all edges
        for (auto it : myEdges) {
            for (auto it_lane : it.second->getLanes()) {
                result.push_back(it_lane);
            }
        }
    }
    return result;
}


GNELane*
GNENet::retrieveLane(const std::string& id, bool failHard, bool checkVolatileChange) {
    const std::string edge_id = SUMOXMLDefinitions::getEdgeIDFromLane(id);
    GNEEdge* edge = retrieveEdge(edge_id, failHard);
    if (edge != 0) {
        GNELane* lane = NULL;
        // search  lane in lane's edges
        for (auto it : edge->getLanes()) {
            if (it->getID() == id) {
                lane = it;
            }
        }
        // throw exception or return NULL if lane wasn't found
        if (lane == NULL) {
            if (failHard) {
                // Throw exception if failHard is enabled
                throw UnknownElement(toString(SUMO_TAG_LANE) + " " + id);
            }
        } else {
            // check if the recomputing with volatile option has changed the number of lanes (needed for additionals)
            if (checkVolatileChange && (myEdgesAndNumberOfLanes.count(edge_id) == 1) && myEdgesAndNumberOfLanes[edge_id] != (int)edge->getLanes().size()) {
                return edge->getLanes().at(lane->getIndex() + 1);
            }
            return lane;
        }
    } else if (failHard) {
        // Throw exception if failHard is enabled
        throw UnknownElement(toString(SUMO_TAG_EDGE) + " " + edge_id);
    }
    return NULL;
}


std::vector<GNEJunction*>
GNENet::retrieveJunctions(bool onlySelected) {
    std::vector<GNEJunction*> result;
    if(onlySelected) {
        // only returns selected junctions
        for (auto i : mySelectedAttributeCarriers[GLO_JUNCTION]) {
            result.push_back(dynamic_cast<GNEJunction*>(i));
        }
    } else {
        // returns all edges
        for (auto i : myJunctions) {
            result.push_back(i.second);
        }
    }
    return result;
}


std::vector<GNEShape*>
GNENet::retrieveShapes(SumoXMLTag shapeTag, bool onlySelected) {
    std::vector<GNEShape*> result;
    // fill polygons (SUMO_TAG_NOTHING is for return all polygons and POIS)
    if ((shapeTag == SUMO_TAG_NOTHING) || (shapeTag == SUMO_TAG_POLY)) {
         if(onlySelected) {
            // only returns selected polygons
            for (auto i : mySelectedAttributeCarriers[GLO_POLYGON]) {
                result.push_back(dynamic_cast<GNEPoly*>(i));
            }
        } else {
             // return all polygons
            for (const auto& it : getPolygons()) {
                result.push_back(dynamic_cast<GNEPoly*>(it.second));
            }
        }
    }
    // fill POIs
    if ((shapeTag == SUMO_TAG_NOTHING) || (shapeTag == SUMO_TAG_POI)) {
         if(onlySelected) {
            // only returns selected POIs
            for (auto i : mySelectedAttributeCarriers[GLO_POI]) {
                result.push_back(dynamic_cast<GNEPOI*>(i));
            }
        } else {
             // return all POIs
            for (const auto& it : getPOIs()) {
                result.push_back(dynamic_cast<GNEPOI*>(it.second));
            }
        }
    }
    return result;
}


void
GNENet::refreshElement(GUIGlObject* o) {
    myGrid.removeAdditionalGLObject(o);
    myGrid.addAdditionalGLObject(o);
    update();
}


std::string
GNENet::generateVaporizerID() const {
    int counter = 0;
    while (myAdditionals.find(std::pair<std::string, SumoXMLTag>("vaporizer_" + toString(counter), SUMO_TAG_VAPORIZER)) != myAdditionals.end()) {
        counter++;
    }
    return "vaporizer_" + toString(counter);
}


GNEAttributeCarrier*
GNENet::retrieveAttributeCarrier(GUIGlID id, bool failHard) {
    // obtain blocked GUIGlObject
    GUIGlObject* object = GUIGlObjectStorage::gIDStorage.getObjectBlocking(id);
    // Make sure that object exists
    if (object != NULL) {
        // unblock and try to parse to AtributeCarrier
        GUIGlObjectStorage::gIDStorage.unblockObject(id);
        GNEAttributeCarrier* ac = dynamic_cast<GNEAttributeCarrier*>(object);
        // If was sucesfully parsed, return it
        if (ac == NULL) {
            throw ProcessError("GUIGlObject does not match the declared type");
        } else {
            return ac;
        }
    } else if (failHard) {
        return NULL;
    } else {
        throw ProcessError("Attempted to retrieve non-existant GUIGlObject");
    }
}


std::vector<GNEAttributeCarrier*>
GNENet::retrieveAttributeCarriers(SumoXMLTag type) {
    std::vector<GNEAttributeCarrier*> result;
    /**
     myViewNet->getNet()->retrieveAttributeCarriers();
    myViewNet->getNet()->retrieveJunctions();
    myViewNet->getNet()->retrieveEdges();
    myViewNet->getNet()->retrieveLanes();
    myViewNet->getNet()->retrieveConnections();
    myViewNet->getNet()->retrieveAdditionals();
    myViewNet->getNet()->retrieveCrossings();
    myViewNet->getNet()->getPolygons();
    myViewNet->getNet()->getPOIs();

    // iterate over GUIGLIdsd
    for (auto it : ids) {
        // obtain attribute carrier vinculated to GLID
        GNEAttributeCarrier* ac = retrieveAttributeCarrier(it);
        if (ac->getGUIGLObject()->getType() == type) {
            result.push_back(ac);
        }
    }
    */
    return result;
}


void
GNENet::computeEverything(GNEApplicationWindow* window, bool force, bool volatileOptions, std::string additionalPath, std::string shapePath) {
    if (!myNeedRecompute) {
        if (force) {
            if (volatileOptions) {
                window->setStatusBarText("Forced computing junctions with volatile options ...");
            } else {
                window->setStatusBarText("Forced computing junctions ...");
            }
        } else {
            return;
        }
    } else {
        if (volatileOptions) {
            window->setStatusBarText("Computing junctions with volatile options ...");
        } else {
            window->setStatusBarText("Computing junctions  ...");
        }
    }
    // save current number of lanes for every edge if recomputing is with volatile options
    if (volatileOptions) {
        for (auto it : myEdges) {
            myEdgesAndNumberOfLanes[it.second->getID()] = (int)it.second->getLanes().size();
        }
    }

    // compute
    OptionsCont& oc = OptionsCont::getOptions();
    computeAndUpdate(oc, volatileOptions);

    // load additionals if was recomputed with volatile options
    if (additionalPath != "") {
        // Create additional handler
        GNEAdditionalHandler additionalHandler(additionalPath, myViewNet, false);
        // Run parser
        if (!XMLSubSys::runParser(additionalHandler, additionalPath, false)) {
            WRITE_MESSAGE("Loading of " + additionalPath + " failed.");
        } else {
            // update view
            update();
        }
        // clear myEdgesAndNumberOfLanes after reload additionals
        myEdgesAndNumberOfLanes.clear();
    }
    // load shapes if was recomputed with volatile options
    if (shapePath != "") {
        GNEApplicationWindow::GNEShapeHandler handler(shapePath, this);
        if (!XMLSubSys::runParser(handler, shapePath, false)) {
            WRITE_MESSAGE("Loading of " + shapePath + " failed.");
        }
    }
    window->getApp()->endWaitCursor();
    window->setStatusBarText("Finished computing junctions.");
    update();
}


void
GNENet::computeJunction(GNEJunction* junction) {
    // recompute tl-logics
    OptionsCont& oc = OptionsCont::getOptions();
    NBTrafficLightLogicCont& tllCont = getTLLogicCont();
    // iterate over traffic lights definitions
    for (auto it : junction->getNBNode()->getControllingTLS()) {
        it->setParticipantsInformation();
        it->setTLControllingInformation();
        tllCont.computeSingleLogic(oc, it);
    }

    // @todo compute connections etc...
}


void
GNENet::requireRecompute() {
    myNeedRecompute = true;
}


bool
GNENet::netHasGNECrossings() const {
    for (auto n : myJunctions) {
        if (n.second->getGNECrossings().size() > 0) {
            return true;
        }
    }
    return false;
}


FXApp*
GNENet::getApp() {
    return myViewNet->getApp();
}


NBNetBuilder*
GNENet::getNetBuilder() const {
    return myNetBuilder;
}


bool
GNENet::joinSelectedJunctions(GNEUndoList* undoList) {
    std::vector<GNEJunction*> selectedJunctions = retrieveJunctions(true);
    if (selectedJunctions.size() < 2) {
        return false;
    }
    EdgeVector allIncoming;
    EdgeVector allOutgoing;
    std::set<NBNode*> cluster;
    for (auto it : selectedJunctions) {
        cluster.insert(it->getNBNode());
        const EdgeVector& incoming = it->getNBNode()->getIncomingEdges();
        allIncoming.insert(allIncoming.end(), incoming.begin(), incoming.end());
        const EdgeVector& outgoing = it->getNBNode()->getOutgoingEdges();
        allOutgoing.insert(allOutgoing.end(), outgoing.begin(), outgoing.end());
    }
    // create new junction
    Position pos;
    Position oldPos;
    bool setTL;
    std::string id;
    TrafficLightType type;
    myNetBuilder->getNodeCont().analyzeCluster(cluster, id, pos, setTL, type);
    // save position
    oldPos = pos;

    // Check that there isn't another junction in the same position as Pos but doesn't belong to cluster
    for (auto i : myJunctions) {
        if ((i.second->getPositionInView() == pos) && (cluster.find(i.second->getNBNode()) == cluster.end())) {
            // show warning in gui testing debug mode
            if (OptionsCont::getOptions().getBool("gui-testing-debug")) {
                WRITE_WARNING("Opening FXMessageBox 'Join non-selected junction'");
            }
            // Ask confirmation to user
            FXuint answer = FXMessageBox::question(getApp(), MBOX_YES_NO,
                                                   ("Position of joined " + toString(SUMO_TAG_JUNCTION)).c_str(), "%s",
                                                   ("There is another unselected " + toString(SUMO_TAG_JUNCTION) + " in the same position of joined " + toString(SUMO_TAG_JUNCTION) +
                                                    + ".\nIt will be joined with the other selected " + toString(SUMO_TAG_JUNCTION) + "s. Continue?").c_str());
            if (answer != 1) { // 1:yes, 2:no, 4:esc
                // write warning if netedit is running in testing mode
                if ((answer == 2) && (OptionsCont::getOptions().getBool("gui-testing-debug"))) {
                    WRITE_WARNING("Closed FXMessageBox 'Join non-selected junction' with 'No'");
                } else if ((answer == 4) && (OptionsCont::getOptions().getBool("gui-testing-debug"))) {
                    WRITE_WARNING("Closed FXMessageBox 'Join non-selected junction' with 'ESC'");
                }
                return false;
            } else {
                // write warning if netedit is running in testing mode
                if (OptionsCont::getOptions().getBool("gui-testing-debug")) {
                    WRITE_WARNING("Closed FXMessageBox 'Join non-selected junction' with 'Yes'");
                }
                // select conflicted junction an join all again
                i.second->setAttribute(GNE_ATTR_SELECTED, "true", undoList);
                return joinSelectedJunctions(undoList);
            }
        }
    }

    // use checkJunctionPosition to avoid conflicts with junction in the same position as others
    while (checkJunctionPosition(pos) == false) {
        pos.setx(pos.x() + 0.1);
        pos.sety(pos.y() + 0.1);
    }

    // start with the join selected junctions
    undoList->p_begin("Join selected " + toString(SUMO_TAG_JUNCTION) + "s");

    // first remove all crossing of the involved junctions and edges
    for (auto i : selectedJunctions) {

        while (i->getGNECrossings().size() > 0) {
            deleteCrossing(i->getGNECrossings().front(), undoList);
        }
    }

    // #3128 this is not undone when calling 'undo'
    myNetBuilder->getNodeCont().registerJoinedCluster(cluster);
    GNEJunction* joined = createJunction(pos, undoList);
    if (setTL) {
        joined->setAttribute(SUMO_ATTR_TYPE, toString(NODETYPE_TRAFFIC_LIGHT), undoList);
        // XXX ticket831
        //joined-><getTrafficLight>->setAttribute(SUMO_ATTR_TYPE, toString(type), undoList);
    }
    // remap edges
    for (auto it : allIncoming) {
        GNEEdge* oldEdge = myEdges[it->getID()];
        remapEdge(oldEdge, oldEdge->getGNEJunctionSource(), joined, undoList, true, true);
    }
    for (auto it : allOutgoing) {
        GNEEdge* oldEdge = myEdges[it->getID()];
        remapEdge(oldEdge, joined, oldEdge->getGNEJunctionDestiny(), undoList, true, true);
    }
    // delete original junctions
    for (auto it : selectedJunctions) {
        deleteJunction(it, undoList);
    }
    joined->setAttribute(SUMO_ATTR_ID, id, undoList);

    // check if joined junction had to change their original position to avoid errors
    if (pos != oldPos) {
        joined->setAttribute(SUMO_ATTR_POSITION, toString(oldPos), undoList);
    }
    undoList->p_end();
    return true;
}


bool
GNENet::cleanInvalidCrossings(GNEUndoList* undoList) {
    // obtain current net's crossings
    std::vector<GNECrossing*> myNetCrossings;
    for (auto it : myJunctions) {
        myNetCrossings.reserve(myNetCrossings.size() + it.second->getGNECrossings().size());
        myNetCrossings.insert(myNetCrossings.end(), it.second->getGNECrossings().begin(), it.second->getGNECrossings().end());
    }
    // obtain invalid crossigns
    std::vector<GNECrossing*> myInvalidCrossings;
    for (auto i = myNetCrossings.begin(); i != myNetCrossings.end(); i++) {
        if ((*i)->getNBCrossing()->valid == false) {
            myInvalidCrossings.push_back(*i);
        }
    }

    if (myInvalidCrossings.empty()) {
        // show warning in gui testing debug mode
        if (OptionsCont::getOptions().getBool("gui-testing-debug")) {
            WRITE_WARNING("Opening FXMessageBox 'No crossing to remove'");
        }
        // open a dialog informing that there isn't crossing to remove
        FXMessageBox::warning(getApp(), MBOX_OK,
                              ("Clear " + toString(SUMO_TAG_CROSSING) + "s").c_str(), "%s",
                              ("There is no invalid " + toString(SUMO_TAG_CROSSING) + "s to remove").c_str());
        // show warning in gui testing debug mode
        WRITE_WARNING("Closed FXMessageBox 'No crossing to remove' with 'OK'");
    } else {
        std::string plural = myInvalidCrossings.size() == 1 ? ("") : ("s");
        // show warning in gui testing debug mode
        if (OptionsCont::getOptions().getBool("gui-testing-debug")) {
            WRITE_WARNING("Opening FXMessageBox 'clear crossings'");
        }
        // Ask confirmation to user
        FXuint answer = FXMessageBox::question(getApp(), MBOX_YES_NO,
                                               ("Clear " + toString(SUMO_TAG_CROSSING) + "s").c_str(), "%s",
                                               ("Clear " + toString(SUMO_TAG_CROSSING) + plural + " will be removed. Continue?").c_str());
        if (answer != 1) { // 1:yes, 2:no, 4:esc
            // write warning if netedit is running in testing mode
            if ((answer == 2) && (OptionsCont::getOptions().getBool("gui-testing-debug"))) {
                WRITE_WARNING("Closed FXMessageBox 'clear crossings' with 'No'");
            } else if ((answer == 4) && (OptionsCont::getOptions().getBool("gui-testing-debug"))) {
                WRITE_WARNING("Closed FXMessageBox 'clear crossings' with 'ESC'");
            }
        } else {
            undoList->p_begin("Clean " + toString(SUMO_TAG_CROSSING) + "s");
            for (auto i = myInvalidCrossings.begin(); i != myInvalidCrossings.end(); i++) {
                deleteCrossing((*i), undoList);
            }
            undoList->p_end();
        }
    }
    return 1;
}


void
GNENet::removeSolitaryJunctions(GNEUndoList* undoList) {
    undoList->p_begin("Clean " + toString(SUMO_TAG_JUNCTION) + "s");
    std::vector<GNEJunction*> toRemove;
    for (auto it : myJunctions) {
        GNEJunction* junction = it.second;
        if (junction->getNBNode()->getEdges().size() == 0) {
            toRemove.push_back(junction);
        }
    }
    for (auto it : toRemove) {
        deleteJunction(it, undoList);
    }
    undoList->p_end();
}


void
GNENet::replaceJunctionByGeometry(GNEJunction* junction, GNEUndoList* undoList) {
    assert(junction->getNBNode()->checkIsRemovable());
    // start operation
    undoList->p_begin("Replace junction by geometry");
    // obtain Edges to join
    std::vector<std::pair<NBEdge*, NBEdge*> > toJoin = junction->getNBNode()->getEdgesToJoin();
    // clear connections of junction to replace
    clearJunctionConnections(junction, undoList);
    // iterate over NBEdges to join
    for (auto j : toJoin) {
        // obtain GNEEdges
        GNEEdge* begin = myEdges[j.first->getID()];
        GNEEdge* continuation = myEdges[j.second->getID()];
        // remove connections between the edges
        std::vector<NBEdge::Connection> connections = begin->getNBEdge()->getConnections();
        for (auto con : connections) {
            undoList->add(new GNEChange_Connection(begin, con, false, false), true);
        }
        // replace incoming edge
        replaceIncomingEdge(continuation, begin, undoList);
        // fix shape of replaced edge
        PositionVector newShape = begin->getNBEdge()->getInnerGeometry();
        if (begin->getNBEdge()->hasDefaultGeometryEndpointAtNode(begin->getNBEdge()->getToNode())) {
            newShape.push_back(junction->getNBNode()->getPosition());
        } else {
            newShape.push_back(begin->getNBEdge()->getGeometry()[-1]);
        }
        if (continuation->getNBEdge()->hasDefaultGeometryEndpointAtNode(begin->getNBEdge()->getToNode())) {
            newShape.push_back_noDoublePos(junction->getNBNode()->getPosition());
        } else {
            newShape.push_back_noDoublePos(continuation->getNBEdge()->getGeometry()[0]);
        }
        newShape.append(continuation->getNBEdge()->getInnerGeometry());
        begin->setAttribute(GNE_ATTR_SHAPE_END, continuation->getAttribute(GNE_ATTR_SHAPE_END), undoList);
        begin->setAttribute(SUMO_ATTR_SHAPE, toString(newShape), undoList);
    }
    //delete replaced junction
    deleteJunction(junction, undoList);
    // finish operation
    undoList->p_end();
}


void
GNENet::clearJunctionConnections(GNEJunction* junction, GNEUndoList* undoList) {
    undoList->p_begin("clear junction connections");
    std::vector<GNEConnection*> connections = junction->getGNEConnections();
    // Iterate over all connections and clear it
    for (auto i : connections) {
        deleteConnection(i, undoList);
    }
    undoList->p_end();
}


void
GNENet::resetJunctionConnections(GNEJunction* junction, GNEUndoList* undoList) {
    undoList->p_begin("reset junction connections");
    // first clear connections
    clearJunctionConnections(junction, undoList);
    // invalidate logic to create new connections in the next recomputing
    junction->setLogicValid(false, undoList);
    undoList->p_end();
}


void
GNENet::renameEdge(GNEEdge* edge, const std::string& newID) {
    myEdges.erase(edge->getNBEdge()->getID());
    myNetBuilder->getEdgeCont().rename(edge->getNBEdge(), newID);
    edge->setMicrosimID(newID);
    myEdges[newID] = edge;
    // rename all connections related to this edge
    for (auto i : edge->getLanes()) {
        i->updateConnectionIDs();
    }
}


void
GNENet::changeEdgeEndpoints(GNEEdge* edge, const std::string& newSource, const std::string& newDest) {
    NBNode* from = retrieveJunction(newSource)->getNBNode();
    NBNode* to = retrieveJunction(newDest)->getNBNode();
    edge->getNBEdge()->reinitNodes(from, to);
    requireRecompute();
    update();
}


GNEViewNet*
GNENet::getViewNet() const {
    return myViewNet;
}


std::vector<GNEAttributeCarrier*>
GNENet::getSelectedAttributeCarriers() const {
    std::vector<GNEAttributeCarrier*> result;
    for(auto i : mySelectedAttributeCarriers) {
        result.reserve(result.size() + i.second.size());
        std::move(i.second.begin(), i.second.end(), std::back_inserter(result));
    }
    return result;
}


const std::vector<GNEAttributeCarrier*> &
GNENet::getSelectedAttributeCarriers(GUIGlObjectType type) {
    return mySelectedAttributeCarriers[type];
}


void 
GNENet::selectAttributeCarrier(GNEAttributeCarrier* attributeCarrier) {
    if(attributeCarrier == NULL) {
        throw ProcessError("AttributeCarrier cannot be NULL");
    } else {
        GUIGlObjectType glType = attributeCarrier->getGUIGLObject()->getType();
        if(std::find(mySelectedAttributeCarriers[glType].begin(), mySelectedAttributeCarriers[glType].end(), attributeCarrier) == mySelectedAttributeCarriers[glType].end()) {
            mySelectedAttributeCarriers[glType].push_back(attributeCarrier);
        } else {
            throw ProcessError("AttributeCarrier isn't selected");
        }
    }
}


void 
GNENet::unselectAttributeCarrier(GNEAttributeCarrier* attributeCarrier) {
    if(attributeCarrier == NULL) {
        throw ProcessError("AttributeCarrier cannot be NULL");
    } else {
        GUIGlObjectType glType = attributeCarrier->getGUIGLObject()->getType();
        std::vector<GNEAttributeCarrier*>::iterator it = std::find(mySelectedAttributeCarriers[glType].begin(), mySelectedAttributeCarriers[glType].end(), attributeCarrier);
        if(it != mySelectedAttributeCarriers[glType].end()) {
            mySelectedAttributeCarriers[glType].erase(it);
        } else {
            throw ProcessError("AttributeCarrier isn't selected");
        }
    }
}


NBTrafficLightLogicCont&
GNENet::getTLLogicCont() {
    return myNetBuilder->getTLLogicCont();
}


void
GNENet::renameJunction(GNEJunction* junction, const std::string& newID) {
    myJunctions.erase(junction->getNBNode()->getID());
    myNetBuilder->getNodeCont().rename(junction->getNBNode(), newID);
    junction->setMicrosimID(newID);
    myJunctions[newID] = junction;
}


void
GNENet::addExplicitTurnaround(std::string id) {
    myExplicitTurnarounds.insert(id);
}


void
GNENet::removeExplicitTurnaround(std::string id) {
    myExplicitTurnarounds.erase(id);
}


GNEAdditional*
GNENet::retrieveAdditional(const std::string& id, bool hardFail) const {
    for (auto i : myAdditionals) {
        if (i.second->getID() == id) {
            return i.second;
        }
    }
    if (hardFail) {
        throw ProcessError("Attempted to retrieve non-existant additional");
    } else {
        return NULL;
    }
}


std::vector<GNEAdditional*>
GNENet::retrieveAdditionals(bool onlySelected) {
    std::vector<GNEAdditional*> result;
    if(onlySelected) {
        // only returns selected additionals
        for (auto i : mySelectedAttributeCarriers[GLO_ADDITIONAL]) {
            result.push_back(dynamic_cast<GNEAdditional*>(i));
        }
    } else {
        // returns all additionals
        for (auto i : myAdditionals) {
            result.push_back(i.second);
        }
    }
    return result;
}


GNEAdditional*
GNENet::getAdditional(SumoXMLTag type, const std::string& id) const {
    if (myAdditionals.empty()) {
        return NULL;
    } else if (myAdditionals.find(std::pair<std::string, SumoXMLTag>(id, type)) != myAdditionals.end())  {
        return myAdditionals.at(std::pair<std::string, SumoXMLTag>(id, type));
    } else {
        return NULL;
    }
}


std::vector<GNEAdditional*>
GNENet::getAdditionals(SumoXMLTag type) const {
    std::vector<GNEAdditional*> vectorOfAdditionals;
    for (auto i : myAdditionals) {
        if ((type == SUMO_TAG_NOTHING) || (type == i.second->getTag())) {
            vectorOfAdditionals.push_back(i.second);
        }
    }
    return vectorOfAdditionals;
}


GNERerouterInterval*
GNENet::getRerouterInterval(const std::string& rerouterIntervalID) const {
    // iterate over additionals and obtain Rerouters
    for (auto i : myAdditionals) {
        if (i.second->getTag() == SUMO_TAG_REROUTER) {
            GNERerouter* rerouter = dynamic_cast<GNERerouter*>(i.second);
            // iterate over intervals of rerouter.
            for (auto j : rerouter->getRerouterIntervals()) {
                if (j->getID() == rerouterIntervalID) {
                    return j;
                }
            }
        }
    }
    return NULL;
}


int
GNENet::getNumberOfAdditionals(SumoXMLTag type) const {
    int counter = 0;
    for (auto i : myAdditionals) {
        if ((type == SUMO_TAG_NOTHING) || (type == i.second->getTag())) {
            counter++;
        }
    }
    return counter;
}


void
GNENet::updateAdditionalID(const std::string& oldID, GNEAdditional* additional) {
    auto additionalToUpdate = myAdditionals.find(std::pair<std::string, SumoXMLTag>(oldID, additional->getTag()));
    if (additionalToUpdate == myAdditionals.end()) {
        throw ProcessError(toString(additional->getTag()) + "  with old ID='" + oldID + "' doesn't exist");
    } else {
        // remove an insert additional again into container
        myAdditionals.erase(additionalToUpdate);
        myAdditionals[std::pair<std::string, SumoXMLTag>(additional->getID(), additional->getTag())] = additional;
        // additionals has to be saved
        requiereSaveAdditionals();
    }
}


void
GNENet::requiereSaveAdditionals() {
    if ((myAdditionalsSaved == true) && OptionsCont::getOptions().getBool("gui-testing-debug")) {
        WRITE_WARNING("Additionals has to be saved");
    }
    myAdditionalsSaved = false;
    myViewNet->getViewParent()->getGNEAppWindows()->enableSaveAdditionalsMenu();
}


void
GNENet::saveAdditionals(const std::string& filename) {
    // obtain invalid stopping places and detectors
    std::vector<GNEStoppingPlace*> invalidStoppingPlaces;
    std::vector<GNEDetector*> invalidDetectors;
    for (auto i : myAdditionals) {
        GNEStoppingPlace* stoppingPlace = dynamic_cast<GNEStoppingPlace*>(i.second);
        GNEDetector* detector = dynamic_cast<GNEDetector*>(i.second);
        // check if has to be fixed
        if ((stoppingPlace != NULL) && (stoppingPlace->areStoppingPlacesPositionsFixed() == false)) {
            invalidStoppingPlaces.push_back(stoppingPlace);
        } else if ((detector != NULL) && (detector->isDetectorPositionFixed() == false)) {
            invalidDetectors.push_back(detector);
        }
    }
    // if there are invalid StoppingPlaces or detectors, open GNEDialog_FixAdditionalPositions
    if (invalidStoppingPlaces.size() > 0 || invalidDetectors.size() > 0) {
        // 0 -> Canceled Saving, with or whithout selecting invalid stopping places and E2
        // 1 -> Invalid stoppingPlaces and E2 fixed, friendlyPos enabled, or saved with invalid positions
        GNEDialog_FixAdditionalPositions fixAdditionalPositionsDialog(myViewNet, invalidStoppingPlaces, invalidDetectors);
        if (fixAdditionalPositionsDialog.execute() == 0) {
            // Here a console message
            ;
        } else {
            // save additionals
            OutputDevice& device = OutputDevice::getDevice(filename);
            device.openTag("additionals");
            for (auto i : myAdditionals) {
                i.second->writeAdditional(device);
            }
            device.close();
        }
        // set focus again in viewNet
        myViewNet->setFocus();
    } else {
        OutputDevice& device = OutputDevice::getDevice(filename);
        device.openTag("additionals");
        for (auto i : myAdditionals) {
            // only save additionals that doesn't have Additional parents
            if (i.second->getAdditionalParent() == NULL) {
                i.second->writeAdditional(device);
            }
        }
        device.close();
    }
    // change value of flag
    myAdditionalsSaved = true;
    // show debug information
    if (OptionsCont::getOptions().getBool("gui-testing-debug")) {
        WRITE_WARNING("Additionals saved");
    }
}


GNECalibratorRoute*
GNENet::retrieveCalibratorRoute(const std::string& id, bool hardFail) const {
    // iterate over calibrator routes
    for (auto i : myCalibratorRoutes) {
        if (i.second->getID() == id) {
            return i.second;
        }
    }
    if (hardFail) {
        throw ProcessError("Attempted to retrieve non-existant calibrator route");
    } else {
        return NULL;
    }
}


GNECalibratorVehicleType*
GNENet::retrieveCalibratorVehicleType(const std::string& id, bool hardFail) const {
    // iterate over vehicle types
    for (auto i : myCalibratorVehicleTypes) {
        if (i.second->getID() == id) {
            return i.second;
        }
    }
    if (hardFail) {
        throw ProcessError("Attempted to retrieve non-existant calibrator vehicle type");
    } else {
        return NULL;
    }
}

std::string
GNENet::generateCalibratorRouteID() const {
    int counter = 0;
    while (myCalibratorRoutes.count(toString(SUMO_TAG_ROUTE) + toString(counter)) != 0) {
        counter++;
    }
    return toString(SUMO_TAG_ROUTE) + toString(counter);
}


std::string
GNENet::generateCalibratorVehicleTypeID() const {
    int counter = 0;
    while (myCalibratorVehicleTypes.count(toString(SUMO_TAG_VTYPE) + toString(counter)) != 0) {
        counter++;
    }
    return toString(SUMO_TAG_VTYPE) + toString(counter);
}


void
GNENet::changeCalibratorRouteID(GNECalibratorRoute* route, const std::string& oldID) {
    if (myCalibratorRoutes.count(oldID) > 0) {
        myCalibratorRoutes.erase(oldID);
        myCalibratorRoutes[route->getID()] = route;
    } else {
        throw ProcessError("Route wasn't inserted");
    }
}


void
GNENet::changeCalibratorVehicleTypeID(GNECalibratorVehicleType* vehicleType, const std::string& oldID) {
    if (myCalibratorVehicleTypes.count(oldID) > 0) {
        myCalibratorVehicleTypes.erase(oldID);
        myCalibratorVehicleTypes[vehicleType->getID()] = vehicleType;
    } else {
        throw ProcessError("VehicleType wasn't inserted");
    }
}


GNEPoly*
GNENet::addPolygonForEditShapes(GNENetElement* netElement, const PositionVector& shape, bool fill, RGBColor col) {
    if (shape.size() > 0) {
        // create poly for edit shapes
        GNEPoly* shapePoly = new GNEPoly(this, "edit_shape", "edit_shape", shape, false, true, col, GLO_POLYGON, 0, "", false, false , false);
        shapePoly->setShapeEditedElement(netElement);
        shapePoly->setFill(fill);
        shapePoly->setLineWidth(0.3);
        myGrid.addAdditionalGLObject(shapePoly);
        myViewNet->update();
        return shapePoly;
    } else {
        throw ProcessError("shape cannot be empty");
    }
}


void
GNENet::removePolygonForEditShapes(GNEPoly* polygon) {
    if (polygon) {
        // remove it from Inspector Frame
        myViewNet->getViewParent()->getInspectorFrame()->removeInspectedAC(polygon);
        // Remove from grid
        myGrid.removeAdditionalGLObject(polygon->getGUIGLObject());
        myViewNet->update();
    } else {
        throw ProcessError("Polygon for edit shapes has to be inicializated");
    }
}


std::string
GNENet::generateShapeID(SumoXMLTag shapeTag) const {
    // generate tag depending of type of shape
    if (shapeTag == SUMO_TAG_POLY) {
        int counter = 0;
        std::string newID = "poly_" + toString(counter);
        // generate new IDs to find a non-assigned ID
        while (myPolygons.get(newID) != NULL) {
            counter++;
            newID = "poly_" + toString(counter);
        }
        return newID;
    } else {
        int counter = 0;
        std::string newID = "POI_" + toString(counter);
        // generate new IDs to find a non-assigned ID
        while (myPOIs.get(newID) != NULL) {
            counter++;
            newID = "POI_" + toString(counter);
        }
        return newID;
    }
}


void
GNENet::changeShapeID(GNEShape* s, const std::string& OldID) {
    if (s->getTag() == SUMO_TAG_POLY) {
        if (myPolygons.get(OldID) == 0) {
            throw UnknownElement("Polygon " + OldID);
        } else {
            myPolygons.changeID(OldID, s->getID());
        }
    } else {
        if (myPOIs.get(OldID) == 0) {
            throw UnknownElement("POI " + OldID);
        } else {
            myPOIs.changeID(OldID, s->getID());
        }
    }
}


void
GNENet::requiereSaveShapes() {
    if ((myShapesSaved == true) && OptionsCont::getOptions().getBool("gui-testing-debug")) {
        WRITE_WARNING("Shapes has to be saved");
    }
    myShapesSaved = false;
    myViewNet->getViewParent()->getGNEAppWindows()->enableSaveShapesMenu();
}


void 
GNENet::saveShapes(const std::string& filename) {
    // save Shapes
    OutputDevice& device = OutputDevice::getDevice(filename);
    device.openTag("additionals");
    // write only visible polygons
    for (const auto& i : myPolygons) {
        dynamic_cast<GNEShape*>(i.second)->writeShape(device);
    }
    // write only visible POIs
    for (const auto& i : myPOIs) {
        dynamic_cast<GNEShape*>(i.second)->writeShape(device);
    }
    device.close();
    // change flag to true
    myShapesSaved = true;
    // show debug information
    if (OptionsCont::getOptions().getBool("gui-testing-debug")) {
        WRITE_WARNING("Shapes saved");
    }
}


int
GNENet::getNumberOfShapes() const {
    return (int)(myPolygons.size() + myPOIs.size());
}


void 
GNENet::requiereSaveTLSPrograms() {
    if ((myTLSProgramsSaved == true) && OptionsCont::getOptions().getBool("gui-testing-debug")) {
        WRITE_WARNING("TLSPrograms has to be saved");
    }
    myTLSProgramsSaved = false;
    myViewNet->getViewParent()->getGNEAppWindows()->enableSaveTLSProgramsMenu();
}


void 
GNENet::saveTLSPrograms(const std::string& filename) {
    // open output device
    OutputDevice& device = OutputDevice::getDevice(filename);
    device.openTag("additionals");
    // write traffic lights using NWWriter
    NWWriter_SUMO::writeTrafficLights(device, getTLLogicCont());
    device.close();
    // change flag to true
    myTLSProgramsSaved = true;
    // show debug information
    if (OptionsCont::getOptions().getBool("gui-testing-debug")) {
        WRITE_WARNING("TLSPrograms saved");
    }
}


int 
GNENet::getNumberOfTLSPrograms() const {
    return -1;
}


void
GNENet::insertAdditional(GNEAdditional* additional) {
    // Check if additional element exists before insertion
    if (myAdditionals.find(std::pair<std::string, SumoXMLTag>(additional->getID(), additional->getTag())) == myAdditionals.end()) {
        myAdditionals[std::pair<std::string, SumoXMLTag>(additional->getID(), additional->getTag())] = additional;
        myGrid.addAdditionalGLObject(additional);
        // update geometry after insertion of additionals
        additional->updateGeometry();
        // additionals has to be saved
        requiereSaveAdditionals();
    } else {
        throw ProcessError(toString(additional->getTag()) + " with ID='" + additional->getID() + "' already exist");
    }
}


void
GNENet::deleteAdditional(GNEAdditional* additional) {
    // obtain iterator to additional to remove
    auto additionalToRemove = myAdditionals.find(std::pair<std::string, SumoXMLTag>(additional->getID(), additional->getTag()));
    // Check if additional element exists before deletion
    if (additionalToRemove == myAdditionals.end()) {
        throw ProcessError(toString(additional->getTag()) + " with ID='" + additional->getID() + "' doesn't exist");
    } else {
        // remove it from Inspector Frame
        myViewNet->getViewParent()->getInspectorFrame()->removeInspectedAC(additional);
        // Remove from container
        myAdditionals.erase(additionalToRemove);
        // Remove from grid
        myGrid.removeAdditionalGLObject(additional);
        // update view
        update();
        // additionals has to be saved
        requiereSaveAdditionals();
    }
}


void
GNENet::insertCalibratorRoute(GNECalibratorRoute* route) {
    if (myCalibratorRoutes.find(route->getID()) == myCalibratorRoutes.end()) {
        myCalibratorRoutes[route->getID()] = route;
    } else {
        throw ProcessError("Route already inserted");
    }
}


void
GNENet::deleteCalibratorRoute(GNECalibratorRoute* route) {
    auto it = myCalibratorRoutes.find(route->getID());
    if (it != myCalibratorRoutes.end()) {
        myCalibratorRoutes.erase(it);
    } else {
        throw ProcessError("Route wasn't inserted");
    }
}


void
GNENet::insertCalibratorVehicleType(GNECalibratorVehicleType* vehicleType) {
    if (myCalibratorVehicleTypes.find(vehicleType->getID()) == myCalibratorVehicleTypes.end()) {
        myCalibratorVehicleTypes[vehicleType->getID()] = vehicleType;
    } else {
        throw ProcessError("Vehicle Type already inserted");
    }
}


void
GNENet::deleteCalibratorVehicleType(GNECalibratorVehicleType* vehicleType) {
    auto it = myCalibratorVehicleTypes.find(vehicleType->getID());
    if (it != myCalibratorVehicleTypes.end()) {
        myCalibratorVehicleTypes.erase(it);
    } else {
        throw ProcessError("Vehicle Type wasn't inserted");
    }
}

// ===========================================================================
// private
// ===========================================================================

void
GNENet::initJunctionsAndEdges() {
    // init junctions (by default Crossing and walking areas aren't created)
    NBNodeCont& nodeContainer = myNetBuilder->getNodeCont();
    for (auto name_it : nodeContainer.getAllNames()) {
        NBNode* nbn = nodeContainer.retrieve(name_it);
        registerJunction(new GNEJunction(*nbn, this, true));
    }

    // init edges
    NBEdgeCont& ec = myNetBuilder->getEdgeCont();
    for (auto name_it : ec.getAllNames()) {
        NBEdge* nbe = ec.retrieve(name_it);
        registerEdge(new GNEEdge(*nbe, this, false, true));
        if (myGrid.getWidth() > 10e16 || myGrid.getHeight() > 10e16) {
            throw ProcessError("Network size exceeds 1 Lightyear. Please reconsider your inputs.\n");
        }
    }

    // make sure myGrid is initialized even for an empty net
    if (myEdges.size() == 0) {
        myGrid.add(Boundary(0, 0, 100, 100));
    }

    // sort nodes edges so that arrows can be drawn correctly
    NBNodesEdgesSorter::sortNodesEdges(nodeContainer);
}


void
GNENet::insertJunction(GNEJunction* junction) {
    myNetBuilder->getNodeCont().insert(junction->getNBNode());
    registerJunction(junction);
}


void
GNENet::insertEdge(GNEEdge* edge) {
    NBEdge* nbe = edge->getNBEdge();
    myNetBuilder->getEdgeCont().insert(nbe); // should we ignore pruning double edges?
    // if this edge was previouls extracted from the edgeContainer we have to rewire the nodes
    nbe->getFromNode()->addOutgoingEdge(nbe);
    nbe->getToNode()->addIncomingEdge(nbe);
    registerEdge(edge);
}


GNEJunction*
GNENet::registerJunction(GNEJunction* junction) {
    junction->incRef("GNENet::registerJunction");
    junction->setResponsible(false);
    myJunctions[junction->getMicrosimID()] = junction;
    myGrid.add(junction->getBoundary());
    myGrid.addAdditionalGLObject(junction);
    // @todo let Boundary class track z-coordinate natively
    const double z = junction->getNBNode()->getPosition().z();
    if (z != 0) {
        myZBoundary.add(z, Z_INITIALIZED);
    }
    update();
    return junction;
}


GNEEdge*
GNENet::registerEdge(GNEEdge* edge) {
    edge->incRef("GNENet::registerEdge");
    edge->setResponsible(false);
    // add edge to internal container of GNENet
    myEdges[edge->getMicrosimID()] = edge;
    // add edge to grid
    myGrid.add(edge->getBoundary());
    myGrid.addAdditionalGLObject(edge);
    // Add references into GNEJunctions
    edge->getGNEJunctionSource()->addOutgoingGNEEdge(edge);
    edge->getGNEJunctionDestiny()->addIncomingGNEEdge(edge);
    // update view
    update();
    return edge;
}


void
GNENet::deleteSingleJunction(GNEJunction* junction) {
    // remove it from Inspector Frame
    myViewNet->getViewParent()->getInspectorFrame()->removeInspectedAC(junction);
    // Remove from grid and container
    myGrid.removeAdditionalGLObject(junction);
    myJunctions.erase(junction->getMicrosimID());
    myNetBuilder->getNodeCont().extract(junction->getNBNode());
    junction->decRef("GNENet::deleteSingleJunction");
    junction->setResponsible(true);
    update();
}


void
GNENet::deleteSingleEdge(GNEEdge* edge) {
    // remove it from Inspector Frame
    myViewNet->getViewParent()->getInspectorFrame()->removeInspectedAC(edge);
    // remove edge from visual grid and container
    myGrid.removeAdditionalGLObject(edge);
    myEdges.erase(edge->getMicrosimID());
    // extract edge of district container
    myNetBuilder->getEdgeCont().extract(myNetBuilder->getDistrictCont(), edge->getNBEdge());
    edge->decRef("GNENet::deleteSingleEdge");
    edge->setResponsible(true);
    // Remove refrences from GNEJunctions
    edge->getGNEJunctionSource()->removeOutgoingGNEEdge(edge);
    edge->getGNEJunctionDestiny()->removeIncomingGNEEdge(edge);
    // invalidate junction logic
    update();
}


void
GNENet::insertShape(GNEShape* shape) {
    // add shape to grid
    myGrid.addAdditionalGLObject(shape->getGUIGLObject());
    // add shape depending of their types
    if (shape->getTag() == SUMO_TAG_POLY) {
        myPolygons.add(shape->getID(), dynamic_cast<GUIPolygon*>(shape));
    } else {
        myPOIs.add(shape->getID(), dynamic_cast<GUIPointOfInterest*>(shape));
    }
    // POILanes has to be added from lane
    if (shape->getTag() == SUMO_TAG_POILANE) {
        retrieveLane(shape->getAttribute(SUMO_ATTR_LANE))->addShapeChild(shape);
    }
    // insert shape requieres always save shapes
    requiereSaveShapes();
}


void
GNENet::removeShape(GNEShape* shape) {
    // remove it from Inspector Frame
    myViewNet->getViewParent()->getInspectorFrame()->removeInspectedAC(shape);
    // remove shape from grid
    myGrid.removeAdditionalGLObject(shape->getGUIGLObject());
    // remove shape depending of their types
    if (shape->getTag() == SUMO_TAG_POLY) {
        myPolygons.remove(shape->getID(), false);
    } else {
        myPOIs.remove(shape->getID(), false);
    }
    // POILanes has to be removed from lane
    if (shape->getTag() == SUMO_TAG_POILANE) {
        retrieveLane(shape->getAttribute(SUMO_ATTR_LANE))->removeShapeChild(shape);
    }
    // remove shape requires always save shapes
    requiereSaveShapes();
}


void
GNENet::update() {
    if (myViewNet) {
        myViewNet->update();
    }
}


void
GNENet::reserveEdgeID(const std::string& id) {
    myEdgeIDSupplier.avoid(id);
}


void
GNENet::reserveJunctionID(const std::string& id) {
    myJunctionIDSupplier.avoid(id);
}


void
GNENet::initGNEConnections() {
    for (auto it : myEdges) {
        it.second->remakeGNEConnections();
    }
    for (auto it : myEdges) {
        it.second->updateGeometry();
    }
}


void
GNENet::computeAndUpdate(OptionsCont& oc, bool volatileOptions) {
    // make sure we only add turn arounds to edges which currently exist within the network
    std::set<std::string> liveExplicitTurnarounds;
    for (auto it : myExplicitTurnarounds) {
        if (myEdges.count(it) > 0) {
            liveExplicitTurnarounds.insert(it);
        }
    }

    myNetBuilder->compute(oc, liveExplicitTurnarounds, volatileOptions);
    // update ids if necessary
    if (oc.getBool("numerical-ids") || oc.isSet("reserved-ids")) {
        std::map<std::string, GNEEdge*> newEdgeMap;
        std::map<std::string, GNEJunction*> newJunctionMap;
        // fill newEdgeMap
        for (auto it : myEdges) {
            it.second->setMicrosimID(it.second->getNBEdge()->getID());
            newEdgeMap[it.second->getNBEdge()->getID()] = it.second;
        }
        for (auto it : myJunctions) {
            newJunctionMap[it.second->getNBNode()->getID()] = it.second;
            it.second->setMicrosimID(it.second->getNBNode()->getID());
        }
        myEdges = newEdgeMap;
        myJunctions = newJunctionMap;
    }
    // update rtree if necessary
    if (!oc.getBool("offset.disable-normalization")) {
        for (auto it : myEdges) {
            refreshElement(it.second);
        }
    }
    // Clear current inspected ACs in inspectorFrame if a previous net was loaded
    if (myViewNet != 0) {
        myViewNet->getViewParent()->getInspectorFrame()->clearInspectedAC();
    }
    // Remove from container
    myGrid.reset();
    myGrid.add(GeoConvHelper::getFinal().getConvBoundary());
    // if volatile options are true
    if (volatileOptions) {
        assert(myViewNet != 0);
        // clear all Polys of grid
        for (const auto& i : myPolygons) {
            myGrid.removeAdditionalGLObject(dynamic_cast<GUIGlObject*>(i.second));
        }

        // clear all POIs of grid
        for (const auto& i : myPOIs) {
            myGrid.removeAdditionalGLObject(dynamic_cast<GUIGlObject*>(i.second));
        }

        // clear all additionals of grid
        auto copyOfAdditionals = myAdditionals;
        for (const auto& it : copyOfAdditionals) {
            myGrid.removeAdditionalGLObject(it.second);
        }

        // remove all edges of grid and net
        auto copyOfEdges = myEdges;
        for (auto it : copyOfEdges) {
            myGrid.removeAdditionalGLObject(it.second);
            myEdges.erase(it.second->getMicrosimID());
        }

        // removes all junctions of grid and net
        auto copyOfJunctions = myJunctions;
        for (auto it : copyOfJunctions) {
            myGrid.removeAdditionalGLObject(it.second);
            myJunctions.erase(it.second->getMicrosimID());
        }

        // clear undo list
        myViewNet->getUndoList()->clear();

        // clear additionals (must be do it separated)
        myAdditionals.clear();

        // init again junction an edges
        initJunctionsAndEdges();
    }

    // update precomputed geometries
    initGNEConnections();

    for (const auto& it : myJunctions) {
        // undolist may not yet exist but is also not needed when just marking junctions as valid
        it.second->setLogicValid(true, 0);
        // updated shape
        it.second->updateGeometry();
        refreshElement(it.second);
    }

    myNeedRecompute = false;
}


void
GNENet::replaceInListAttribute(GNEAttributeCarrier* ac, SumoXMLAttr key, const std::string& which, const std::string& by, GNEUndoList* undoList) {
    assert(GNEAttributeCarrier::isList(ac->getTag(), key));
    std::vector<std::string> values = GNEAttributeCarrier::parse<std::vector<std::string> >(ac->getAttribute(key));
    std::vector<std::string> newValues;
    for (auto v : values) {
        newValues.push_back(v == which ? by : v);
    }
    ac->setAttribute(key, toString(newValues), undoList);
}

/****************************************************************************/
