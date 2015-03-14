#ifndef _PRAGMA_COPYRIGHT_
#define _PRAGMA_COPYRIGHT_
#pragma comment(copyright, "%Z% %I% %W% %D% %T%\0")
#endif /* _PRAGMA_COPYRIGHT_ */
/****************************************************************************

* Copyright (c) 2008, 2010 IBM Corporation.
* All rights reserved. This program and the accompanying materials
* are made available under the terms of the Eclipse Public License v1.0s
* which accompanies this distribution, and is available at
* http://www.eclipse.org/legal/epl-v10.html

 Classes: RouterProcessor

 Description: Properties of class 'RouterProcessor':
    input: a. a stream 
           b. a message queue
    output: a set of message queues
    action: route the message to the designated destination
   
 Author: Nicole Nie

 History:
   Date     Who ID    Description
   -------- --- ---   -----------
   02/10/09 nieyy      Initial code (D153875)
   01/16/12 ronglli    Add codes to detect SOCKET_BROKEN

****************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "sci.h"

#include "log.hpp"
#include "exception.hpp"
#include "socket.hpp"

#include "atomic.hpp"
#include "routerproc.hpp"
#include "ctrlblock.hpp"
#include "message.hpp"
#include "queue.hpp"
#include "filter.hpp"
#include "filterlist.hpp"
#include "routinglist.hpp"
#include "topology.hpp"
#include "eventntf.hpp"
#include "privatedata.hpp"
#include "initializer.hpp"
#include "sshfunc.hpp"
#include "writerproc.hpp"
#include "tools.hpp"

RouterProcessor::RouterProcessor(int hndl, RoutingList *rlist, FilterList *flist) 
    : Processor(hndl), curFilterID(SCI_FILTER_NULL), curGroup(SCI_GROUP_ALL), inStream(NULL), routingList(rlist), filterList(flist), joinSegs(false), peerProcessor(NULL)
{
    name = "Router";
    PrivateData *pData = new PrivateData(routingList, filterList, NULL, this);
    setSpecific(pData);
}

RouterProcessor::~RouterProcessor()
{
    if (inQueue)
        delete inQueue;
}

Message * RouterProcessor::read()
{
    assert(inQueue || inStream);

    Message *msg = NULL;
    if (inStream) {
        /* Check every 1 second */
        if (inStream->pollData(1000)) {
            msg = new Message();
            *inStream >> *msg;
        }

        /* Send heartbeat message every 10 seconds */
        counter++;
        if (counter >= 10) {
            WriterProcessor * writer = getPeerProcessor();
            Message *hbMsg = new Message(); 
            hbMsg->build(SCI_FILTER_NULL, SCI_GROUP_ALL, 0, NULL, NULL, Message::HEARTBEAT);
            writer->getInQueue()->produce(hbMsg);
            counter = 0;
        }
    } else {
        msg = inQueue->consume();
    }

    if (msg && (msg->getType() == Message::SEGMENT) && (msg->getFilterID() == SCI_JOIN_SEGMENT)) {
        joinSegs = true;
        msg = Message::joinSegments(msg, inStream, inQueue);
    }

    return msg;
}

void RouterProcessor::process(Message * msg)
{
    Filter *filter = NULL;
    Topology *topo = routingList->getTopology();
    int rc;
    
    switch (msg->getType()) {
        case Message::SEGMENT:
            routingList->bcast(msg->getGroup(), msg);
            break;
        case Message::COMMAND:
            if (msg->getFilterID() == SCI_FILTER_NULL) {
                // bcast the message
                routingList->bcast(msg->getGroup(), msg);
            } else {
                filter = filterList->getFilter(msg->getFilterID());
                if (filter != NULL) {
                    sci_exflag_t exflag = {SCI_NORMAL, NULL};
                    // call user's filter handler
                    curFilterID = msg->getFilterID();
                    curGroup = msg->getGroup();
                    filter->input(msg->getGroup(), msg->getContentBuf(), msg->getContentLen(), &exflag);
                } else {
                    // bcast the message
                    routingList->bcast(msg->getGroup(), msg);
                }
            }
            break;
        case Message::CONFIG:
            topo->unpackMsg(*msg);
            topo->setRoutingList(routingList);
            topo->setFilterList(filterList);
            
            rc = topo->deploy();
            break;
        case Message::FILTER_LOAD:
        case Message::FILTER_UNLOAD:
            if (msg->getType() == Message::FILTER_LOAD) {
                filter = new Filter();
                filter->unpackMsg(*msg);
                rc = filterList->loadFilter(filter->getId(), filter);
            } else {
                rc = filterList->unloadFilter(msg->getFilterID());
            }

            if (gCtrlBlock->getMyRole() == CtrlBlock::FRONT_END) {
                *(int *)gNotifier->getRetVal(msg->getID()) = rc;
                gNotifier->notify(msg->getID());   
            }

            routingList->bcast(SCI_GROUP_ALL, msg);
            break;
        case Message::FILTER_LIST:
            filterList->loadFilterList(*msg);
            break;
        case Message::GROUP_CREATE:
        case Message::GROUP_FREE:
        case Message::GROUP_OPERATE:
        case Message::GROUP_OPERATE_EXT:
        case Message::GROUP_DROP:
        case Message::GROUP_MERGE:
            routingList->parseCmd(msg);
            break;
        case Message::BE_ADD:
            rc = gCtrlBlock->getTopology()->addBE(msg);
            if (gCtrlBlock->getMyRole() == CtrlBlock::FRONT_END) {
                *(int *)gNotifier->getRetVal(msg->getID()) = rc;
                gNotifier->notify(msg->getID());
            }
            break;
        case Message::BE_REMOVE:
            rc = gCtrlBlock->getTopology()->removeBE(msg);
            if (gCtrlBlock->getMyRole() == CtrlBlock::FRONT_END) {
                *(int *)gNotifier->getRetVal(msg->getID()) = rc;
                gNotifier->notify(msg->getID());
            }
            break;
        case Message::QUIT:
            gCtrlBlock->setTermState(true);
            gCtrlBlock->setRecoverMode(0);
            routingList->bcast(SCI_GROUP_ALL, msg);
            setState(false);
            break;
        case Message::RELEASE:
            setState(false);
            break;
        case Message::UNCLE_LIST:
        case Message::ERROR_EVENT:
        case Message::SHUTDOWN:
        case Message::KILLNODE:
            routingList->bcast(SCI_GROUP_ALL, msg);
            break;
        default:
            log_error("Processor %s: received unknown command", name.c_str());
            break;
    }
}

void RouterProcessor::write(Message * msg)
{
    if (joinSegs || inStream) {
        joinSegs = false;
        if (decRefCount(msg->getRefCount()) == 0)
            delete msg;
        return;
    }
        
    inQueue->remove();
}

void RouterProcessor::seize()
{
    try {
        Message *msg = new Message();
        msg->build(SCI_FILTER_NULL, SCI_GROUP_ALL, 0, NULL, NULL, Message::QUIT);
        routingList->bcast(SCI_GROUP_ALL, msg);
    } catch (std::bad_alloc) {
        log_error("Processor Router: out of memory");
        // To do; add correct error handling
    }
    gCtrlBlock->setTermState(true); // do not need to notify the parent the error children
    setState(false);
}

int RouterProcessor::recover()
{
    int rc = -1;

    if ((gCtrlBlock->getTermState()) || (!gCtrlBlock->getRecoverMode())) {
        return rc;
    }

    if (gCtrlBlock->getMyRole() == CtrlBlock::FRONT_END) {
        return rc;
    }

    log_debug("Routerproc: begin to do the recover");
    if (gCtrlBlock->getParentInfoWaitState()) {
        while (gInitializer->pInfoUpdated == false) { 
            if (gCtrlBlock->getTermState()) {
                log_debug("Routerproc: incorrect state");
                return rc;
            }
            SysUtil::sleep(WAIT_INTERVAL);
        }
    }

    while ((rc != 0) && (!gCtrlBlock->getTermState())) {
        try {
            struct iovec sign = {0};
            int hndl = gInitializer->getOrgHandle(); 
            int pID = gInitializer->getParentID();
            string pAddr = gInitializer->getParentAddr();
            int pPort = gInitializer->getParentPort();

            inStream->stopRead();

            WriterProcessor * writer = getPeerProcessor();
            while(!(writer->isLaunched())) {
                SysUtil::sleep(WAIT_INTERVAL);
            }
            
            if (!writer->getRecoverState()) {
                Message *msg = new Message(); 
                msg->build(SCI_FILTER_NULL, SCI_GROUP_ALL, 0, NULL, NULL, Message::RELEASE);

                log_debug("Routerproc: begin to set the writer release state to false");
                writer->setReleaseState(true);
                writer->getInQueue()->produce(msg);
            }
            while(!(writer->getRecoverState())) {
                SysUtil::sleep(WAIT_INTERVAL);
            }
            
            log_debug("Routerproc: Begin to do Recover: My parent host is %s, parent port is %d, parent id is %d", pAddr.c_str(), pPort, pID);
            rc = inStream->init(pAddr.c_str(), pPort);
            if (rc != 0) {
                SysUtil::sleep(WAIT_INTERVAL);
                continue;
            }
            gInitializer->setInStream(inStream);
            psec_sign_data(&sign, "%d%d%d", gCtrlBlock->getJobKey(), hndl, pID);
            *inStream << gCtrlBlock->getJobKey() << hndl << pID << sign << endl;
            *inStream >> endl;

            psec_free_signature(&sign);
            log_debug("Routerproc: Recover: My parent host is %s, parent port is %d, parent id is %d", pAddr.c_str(), pPort, pID);

            writer->setOutStream(inStream);

            if ((rc == 0) && (gCtrlBlock->getParentInfoWaitState())) {
                log_debug("Routerproc: begin to notify %d", gInitializer->notifyID);
                gInitializer->pInfoUpdated = false;
                gCtrlBlock->setParentInfoWaitState(false);
                gNotifier->notify(gInitializer->notifyID);
                log_debug("Routerproc: finish notify %d", gInitializer->notifyID);
            }

        } catch (SocketException &e) {
            rc = -1;
            log_error("Routerproc: socket exception: %s", e.getErrMsg().c_str());
            SysUtil::sleep(WAIT_INTERVAL);
        }
    }

    return rc;
}

void RouterProcessor::clean()
{
    if (inStream)
        inStream->stopRead();
    gCtrlBlock->setFlowctlState(false);
    routingList->stopRouting();
    gCtrlBlock->disable();
}

int RouterProcessor::getCurFilterID()
{
    return curFilterID;
}

sci_group_t RouterProcessor::getCurGroup()
{
    return curGroup;
}

void RouterProcessor::setInQueue(MessageQueue * queue)
{
    inQueue = queue;
}

MessageQueue * RouterProcessor::getInQueue()
{
    return inQueue;
}

void RouterProcessor::setInStream(Stream * stream)
{
    inStream = stream;
}

RoutingList * RouterProcessor::getRoutingList()
{
    return routingList;
}

void RouterProcessor::setPeerProcessor(WriterProcessor * processor)
{
        peerProcessor =  processor;
}

WriterProcessor *RouterProcessor::getPeerProcessor()
{
        return peerProcessor;
}

