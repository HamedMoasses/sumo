//---------------------------------------------------------------------------//
//                        GUIGlObjectStorage.cpp -
//  A storage for retrival of displayed object using a numerical id
//                           -------------------
//  project              : SUMO - Simulation of Urban MObility
//  begin                : Sept 2002
//  copyright            : (C) 2002 by Daniel Krajzewicz
//  organisation         : IVF/DLR http://ivf.dlr.de
//  email                : Daniel.Krajzewicz@dlr.de
//---------------------------------------------------------------------------//

//---------------------------------------------------------------------------//
//
//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation; either version 2 of the License, or
//   (at your option) any later version.
//
//---------------------------------------------------------------------------//
namespace
{
    const char rcsid[] =
    "$Id$";
}
// $Log$
// Revision 1.5  2003/11/18 14:28:14  dkrajzew
// debugged and completed lane merging detectors
//
// Revision 1.4  2003/06/05 11:37:30  dkrajzew
// class templates applied
//
//


/* =========================================================================
 * included modules
 * ======================================================================= */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif // HAVE_CONFIG_H


#include "GUIGlObject.h"
#include <map>
#include "GUIGlObjectStorage.h"
#include <microsim/MSVehicle.h>
#include <guisim/GUIVehicle.h>
#include <iostream> // !!! debug only


/* =========================================================================
 * used namespaces
 * ======================================================================= */
using namespace std; // !!! debug only


/* =========================================================================
 * method definitions
 * ======================================================================= */
GUIGlObjectStorage::GUIGlObjectStorage()
    : myAktID(0)
{
}


GUIGlObjectStorage::~GUIGlObjectStorage()
{
}


void
GUIGlObjectStorage::registerObject(GUIGlObject *object)
{
    _lock.lock();
    object->setGlID(myAktID);
    myMap[myAktID++] = object;
    _lock.unlock();
}


void
GUIGlObjectStorage::registerObject(GUIGlObject *object, size_t id)
{
    _lock.lock();
    object->setGlID(id);
    myMap[id] = object;
    _lock.unlock();
}


size_t
GUIGlObjectStorage::getUniqueID()
{
    _lock.lock();
    return myAktID++;
    _lock.unlock();
}


GUIGlObject *
GUIGlObjectStorage::getObjectBlocking(size_t id)
{
    _lock.lock();
    ObjectMap::iterator i=myMap.find(id);
    if(i==myMap.end()) {
        i = myBlocked.find(id);
        if(i!=myBlocked.end()) {
            GUIGlObject *o = (*i).second;
            _lock.unlock();
            return o;
        }
        _lock.unlock();
        return 0;
    }
    GUIGlObject *o = (*i).second;
    myMap.erase(id);
    myBlocked[id] = o;
    _lock.unlock();
    return o;
}


void
GUIGlObjectStorage::remove(size_t id)
{
    _lock.lock();
    ObjectMap::iterator i=myMap.find(id);
    if(i==myMap.end()) {
        i = myBlocked.find(id);
        assert(i!=myBlocked.end());
        GUIGlObject *o = (*i).second;
        myMap.erase(id);
        my2Delete[id] = o;
        _lock.unlock();
    } else {
        GUIGlObject *o = (*i).second;
        myMap.erase(id);
        MSVehicle::remove(static_cast<GUIVehicle*>(o)->id());
        _lock.unlock();
    }
}


void
GUIGlObjectStorage::clear()
{
    _lock.lock();
    myMap.clear();
    myAktID = 0;
    _lock.unlock();
}


void
GUIGlObjectStorage::unblockObject(size_t id)
{
    _lock.lock();
    ObjectMap::iterator i=myBlocked.find(id);
    assert(i!=myBlocked.end());
    GUIGlObject *o = (*i).second;
    myBlocked.erase(id);
    myMap[id] = o;
    _lock.unlock();
}


/**************** DO NOT DEFINE ANYTHING AFTER THE INCLUDE *****************/
//#ifdef DISABLE_INLINE
//#include "GUIGlObjectStorage.icc"
//#endif

// Local Variables:
// mode:C++
// End:


