// Copyright 2013-2015 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
//
// Copyright(c) 2013-2015, Sandia Corporation
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include <sst_config.h>
#include <sst/core/serialization.h>
#include "directoryController.h"

#include <assert.h>

#include <sst/core/params.h>
#include <sst/core/simulation.h>

#include "memNIC.h"


using namespace SST;
using namespace SST::MemHierarchy;
using namespace std;


const MemEvent::id_type DirectoryController::DirEntry::NO_LAST_REQUEST = std::make_pair((uint64_t)-1, -1);

DirectoryController::DirectoryController(ComponentId_t id, Params &params) :
    Component(id), blocksize(0){
    int debugLevel = params.find_integer("debug_level", 0);
    cacheLineSize = params.find_integer("cache_line_size", 64);
    if(debugLevel < 0 || debugLevel > 10)     _abort(DirectoryController, "Debugging level must be between 0 and 10. \n");
    
    dbg.init("", debugLevel, 0, (Output::output_location_t)params.find_integer("debug", 0));
    printStatsLoc = (Output::output_location_t)params.find_integer("statistics", 0);

    targetCount = 0;

    registerTimeBase("1 ns", true);


    entryCacheMaxSize = (size_t)params.find_integer("entry_cache_size", 32768);
    entryCacheSize = 0;
    int addr = params.find_integer("network_address");
    std::string net_bw = params.find_string("network_bw");

    addrRangeStart  = (uint64_t)params.find_integer("addr_range_start", 0);
    addrRangeEnd    = (uint64_t)params.find_integer("addr_range_end", 0);
    interleaveSize  = (Addr)params.find_integer("interleave_size", 0);
    interleaveSize  *= 1024;
    interleaveStep  = (Addr)params.find_integer("interleave_step", 0);
    interleaveStep  *= 1024;
    protocol        = params.find_string("coherence_protocol", "");
    dbg.debug(_L5_, "Directory controller using protocol: %s\n", protocol.c_str());
    
    int mshrSize    = params.find_integer("mshr_num_entries",-1);
    if (mshrSize == -1) mshrSize = HUGE_MSHR;
    if (mshrSize < 1) dbg.fatal(CALL_INFO, -1, "Invalid param(%s): mshr_num_entries - must be at least 1 or else -1 to indicate a very large MSHR\n", getName().c_str());
    mshr            = new   MSHR(&dbg, mshrSize); 
    
    int directMem = params.find_integer("direct_mem_link",1);
    directMemoryLink = (directMem == 1) ? true : false;

    if(0 == addrRangeEnd) addrRangeEnd = (uint64_t)-1;
    numTargets = 0;
	
    /* Check parameter validity */
    if(! ("MESI" == protocol || "mesi" == protocol || "MSI" == protocol || "msi" == protocol) ) {
	dbg.fatal(CALL_INFO, -1, "Invalid param(%s): coherence_protocol - must be 'MESI' or 'MSI'. You specified: %s\n", getName().c_str(), protocol.c_str());
    }

    if (protocol == "mesi") protocol = "MESI";
    if (protocol == "msi") protocol = "MSI";

    /* Get latencies */
    accessLatency   = (uint64_t)params.find_integer("access_latency_cycles", 0);
    mshrLatency     = (uint64_t)params.find_integer("mshr_latency_cycles", 0);

    /* Set up links/network to cache & memory */
    if (directMemoryLink) {
        memLink = configureLink("memory", "1 ns", new Event::Handler<DirectoryController>(this, &DirectoryController::handleMemoryResponse));
        if (!memLink) {
            dbg.fatal(CALL_INFO, -1, "%s, Error creating link to memory from directory controller\n", getName().c_str());
        }
        MemNIC::ComponentInfo myInfo;
        myInfo.link_port                        = "network";
        myInfo.link_bandwidth                   = net_bw;
        myInfo.num_vcs                          = params.find_integer("network_num_vc", 3);
        myInfo.name                             = getName();
        myInfo.network_addr                     = addr;
        myInfo.type                             = MemNIC::TypeDirectoryCtrl;
        myInfo.link_inbuf_size                  = params.find_string("network_input_buffer_size", "1KB");
        myInfo.link_outbuf_size                 = params.find_string("network_output_buffer_size", "1KB");
        network = new MemNIC(this, myInfo, new Event::Handler<DirectoryController>(this, &DirectoryController::handlePacket));

        MemNIC::ComponentTypeInfo typeInfo;
        typeInfo.rangeStart     = addrRangeStart;
        typeInfo.rangeEnd       = addrRangeEnd;
        typeInfo.interleaveSize = interleaveSize;
        typeInfo.interleaveStep = interleaveStep;
        typeInfo.blocksize      = 0;
        network->addTypeInfo(typeInfo);
    } else {
        memoryName  = params.find_string("net_memory_name", "");
        if (memoryName == "") dbg.fatal(CALL_INFO,-1,"Param not specified(%s): net_memory_name - name of the memory owned by this directory controller\n", getName().c_str());

        MemNIC::ComponentInfo myInfo;
        myInfo.link_port                        = "network";
        myInfo.link_bandwidth                   = net_bw;
        myInfo.num_vcs                          = params.find_integer("network_num_vc", 3);
        myInfo.name                             = getName();
        myInfo.network_addr                     = addr;
        myInfo.type                             = MemNIC::TypeNetworkDirectory;
        myInfo.link_inbuf_size                  = params.find_string("network_input_buffer_size", "1KB");
        myInfo.link_outbuf_size                 = params.find_string("network_output_buffer_size", "1KB");
        network = new MemNIC(this, myInfo, new Event::Handler<DirectoryController>(this, &DirectoryController::handlePacket));
        
        MemNIC::ComponentTypeInfo typeInfo;
        typeInfo.rangeStart     = addrRangeStart;
        typeInfo.rangeEnd       = addrRangeEnd;
        typeInfo.interleaveSize = interleaveSize;
        typeInfo.interleaveStep = interleaveStep;
        typeInfo.blocksize      = 0;
        network->addTypeInfo(typeInfo);
    }
    

    registerClock(params.find_string("clock", "1GHz"), new Clock::Handler<DirectoryController>(this, &DirectoryController::clock));

    // Timestamp - aka cycle count
    timestamp = 0;

    // Clear statistics counters
    numReqsProcessed    = 0;
    totalReplProcessTime = 0;
    totalGetReqProcessTime = 0;
    totalReqProcessTime = 0;
    numCacheHits        = 0;
    mshrHits            = 0;
    GetXReqReceived     = 0;
    GetSExReqReceived   = 0;
    GetSReqReceived     = 0;
    PutMReqReceived     = 0;
    PutEReqReceived     = 0;
    PutSReqReceived     = 0;
    NACKReceived        = 0;
    FetchRespReceived   = 0;
    FetchRespXReceived  = 0;
    PutMRespReceived    = 0;
    PutERespReceived    = 0;
    PutSRespReceived    = 0;
    dataReads           = 0;
    dataWrites          = 0;
    dirEntryReads       = 0;
    dirEntryWrites      = 0;
    NACKSent            = 0;
    InvSent             = 0; 
    FetchInvSent        = 0;
    FetchInvXSent       = 0;
    GetSRespSent        = 0;
    GetXRespSent        = 0;


}



DirectoryController::~DirectoryController(){
    for(std::map<Addr, DirEntry*>::iterator i = directory.begin(); i != directory.end() ; ++i){
        delete i->second;
    }
    directory.clear();
    
    while(workQueue.size()){
        MemEvent *front = workQueue.front();
        delete front;
        workQueue.pop_front();
    }
}



void DirectoryController::handlePacket(SST::Event *event){
    MemEvent *ev = static_cast<MemEvent*>(event);
    ev->setDeliveryTime(getCurrentSimTimeNano());
     
    dbg.debug(_L3_, "RECV %s \tCmd = %s, BaseAddr = 0x%" PRIx64 ", Src = %s, Time = %" PRIu64 "\n", getName().c_str(), CommandString[ev->getCmd()], ev->getBaseAddr(), ev->getSrc().c_str(), getCurrentSimTimeNano());
    if (ev->getCmd() == GetSResp || ev->getCmd() == GetXResp) {
        handleMemoryResponse(event);
    } else if (ev->queryFlag(MemEvent::F_NONCACHEABLE)) {
        dbg.debug(_L5_,"Forwarding noncacheable event to memory: Cmd = %s, BsAddr = 0x%" PRIx64 ", Addr = 0x%" PRIx64 "\n",CommandString[ev->getCmd()],ev->getBaseAddr(),ev->getAddr());
        profileRequestRecv(ev,NULL);
        Addr localAddr       = convertAddressToLocalAddress(ev->getAddr());
        Addr localBaseAddr   = convertAddressToLocalAddress(ev->getBaseAddr());
        noncacheMemReqs[ev->getID()] = make_pair<Addr,Addr>(ev->getBaseAddr(),ev->getAddr());
        ev->setBaseAddr(localBaseAddr);
        ev->setAddr(localAddr);
        profileRequestSent(ev);
        if (directMemoryLink) {
            memLink->send(ev);
        } else {
            ev->setDst(memoryName);
            network->send(ev);
        }
    } else {
        workQueue.push_back(ev);
    }
}

/**
 * Profile requests sent to directory controller
 * Could wrap this in "if printStatLoc" so that it's ignored if we're not printing stats
 */
inline void DirectoryController::profileRequestRecv(MemEvent * event, DirEntry * entry) {
    Command cmd = event->getCmd();
    switch (cmd) {
    case GetX:
        GetXReqReceived++;
        break;
    case GetSEx:
        GetSExReqReceived++;
        break;
    case GetS:
        GetSReqReceived++;
        break;
    case PutM:
        PutMReqReceived++;
        break;
    case PutE:
        PutEReqReceived++;
        break;
    case PutS:
        PutSReqReceived++;
        break;
    default:
        break;

    }
    if (!entry || entry->isCached()) {
        ++numCacheHits;
    }
}
/** 
 * Profile requests sent from directory controller to memory or other caches
 * Could wrap this in "if printStatLoc" so that it's ignored if we're not printing stats
 */
inline void DirectoryController::profileRequestSent(MemEvent * event) {
    Command cmd = event->getCmd();
    switch(cmd) {
    case PutM:
        if (event->getAddr() == 0) dirEntryWrites++;
        else dataWrites++;
        break;
    case GetX:
        if (event->queryFlag(MemEvent::F_NONCACHEABLE)) {
            dataWrites++;
            break;
        }
    case GetSEx:
    case GetS:
        if (event->getAddr() == 0) dirEntryReads++;
        else dataReads++;
        break;
    case FetchInv:
        FetchInvSent++;
        break;
    case FetchInvX:
        FetchInvXSent++;
        break;
    case Inv:
        InvSent++;
        break;
    default:
        break;
        
    }
}

/** 
 * Profile responses sent from directory controller to caches
 * Could wrap this in "if printStatLoc" so that it's ignored if we're not printing stats
 */
inline void DirectoryController::profileResponseSent(MemEvent * event) {
    Command cmd = event->getCmd();
    switch(cmd) {
    case GetSResp:
        GetSRespSent++;
        break;
    case GetXResp:
        GetXRespSent++;
        break;
    case NACK:
        NACKSent++;
        break;
    default:
        break;
    }
}

/** 
 * Profile responses received by directory controller from caches
 * Could wrap this in "if printStatLoc" so that it's ignored if we're not printing stats
 */
inline void DirectoryController::profileResponseRecv(MemEvent * event) {
    Command cmd = event->getCmd();
    switch(cmd) {
    case FetchResp:
        FetchRespReceived++;
        break;
    case FetchXResp:
        FetchRespXReceived++;
        break;
    case PutM:
        PutMRespReceived++;
        break;
    case PutE:
        PutERespReceived++;
        break;
    case PutS:
        PutSRespReceived++;
        break;
    case NACK:
        NACKReceived++;
        break;
    default:
        break;
    }
}   

/**
 *  Called each cycle. Handle any waiting events in the queue.
 */
bool DirectoryController::clock(SST::Cycle_t cycle){
    timestamp++;
    while (!netMsgQueue.empty() && netMsgQueue.begin()->first <= timestamp) {
        MemEvent * ev = netMsgQueue.begin()->second;
        network->send(ev);
        netMsgQueue.erase(netMsgQueue.begin());
    }
    while (!memMsgQueue.empty() && memMsgQueue.begin()->first <= timestamp) {
        MemEvent * ev = memMsgQueue.begin()->second;
        memLink->send(ev);
        memMsgQueue.erase(memMsgQueue.begin());
    }

    network->clock();

    while(!workQueue.empty()){
        MemEvent *event = workQueue.front();
	processPacket(event);
        workQueue.erase(workQueue.begin());
    }

	return false;
}

void DirectoryController::processPacket(MemEvent * ev) {
    dbg.debug(_L3_, "\n\n----------------------------------------------------------------------------------------\n");
    dbg.debug(_L3_, "EVENT: %s, Received: Cmd = %s, BsAddr = 0x%" PRIx64 ", Src = %s, id (%" PRIu64 ",%d), Time = %" PRIu64 "\n",
            getName().c_str(),  CommandString[ev->getCmd()], ev->getBaseAddr(), ev->getSrc().c_str(), 
            ev->getID().first, ev->getID().second, getCurrentSimTimeNano());

    if(! isRequestAddressValid(ev) ) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: Request address is not valid. Addr = 0x%" PRIx64 ", Cmd = %s, Src = %s. Time = %" PRIu64 "ns\n",
                getName().c_str(), ev->getBaseAddr(), CommandString[ev->getCmd()], ev->getSrc().c_str(), getCurrentSimTimeNano());
    }

    Command cmd = ev->getCmd();
    switch (cmd) {
        case GetS:
            handleGetS(ev);
            break;
        case GetSEx:    // handled like GetX, this is a GetS-lock request
        case GetX:
            handleGetX(ev);
            break;
        case NACK:
            handleNACK(ev);
            break;
        case PutS:
            handlePutS(ev);
            break;
        case PutX:
            handlePutX(ev);
            break;
        case PutE:
            handlePutE(ev);
            break;
        case PutM:
            handlePutM(ev);
            break;
        case FetchResp:
            handleFetchResp(ev);
            break;
        case FetchXResp:
            handleFetchXResp(ev);
            break;
        default:
            dbg.fatal(CALL_INFO, -1 , "%s, Error: Received unrecognized request: %s. Addr = 0x%" PRIx64 ", Src = %s. Time = %" PRIu64 "ns\n",
                    getName().c_str(), CommandString[cmd], ev->getBaseAddr(), ev->getSrc().c_str(), getCurrentSimTimeNano());
    }
}

void DirectoryController::handleGetS(MemEvent * ev) {
    /* Locate directory entry and allocate if needed */
    DirEntry * entry = getDirEntry(ev->getBaseAddr());
    if (!entry){
        entry = createDirEntry(ev->getBaseAddr(), ev->getAddr(), ev->getSize());
        entry->setCached(true);   // TODO fix this so new entries go to memory if the cache is not full map, little bit o cheatin here
    }

    /* Put request in MSHR (all GetS requests need to be buffered while they wait for data) and stall if there are waiting requests ahead of us */
    if (!(mshr->elementIsHit(ev->getBaseAddr(), ev))) {
        bool conflict = mshr->isHit(ev->getBaseAddr());
        if (!mshr->insert(ev->getBaseAddr(), ev)) {
            mshrNACKRequest(ev);
            return;
        }
        if (conflict) {
            return; // stall event until conflicting request finishes
        } else {
            profileRequestRecv(ev, entry);
        }
    }

    if (!entry->isCached()) {
        dbg.debug(_L6_, "Entry %" PRIx64 " not in cache.  Requesting from memory.\n", entry->getBaseAddr());
        getDirEntryFromMemory(entry);
        return;
    }

    State state = entry->getState();
    switch (state) {
        case I:
            entry->setState(IS);
            issueMemoryRequest(ev, entry);
            break;
        case S:
            entry->setState(S_D);
            issueMemoryRequest(ev, entry);
            break;
        case M:
            entry->setState(M_InvX);
            issueFetch(ev, entry, FetchInvX);
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "Directory %s received GetS but state is %s\n", getName().c_str(), BccLineString[state]);
    }
}

void DirectoryController::handleGetX(MemEvent * ev) {
    /* Locate directory entry and allocate if needed */
    DirEntry * entry = getDirEntry(ev->getBaseAddr());
    if (!entry){
        entry = createDirEntry(ev->getBaseAddr(), ev->getAddr(), ev->getSize());
        entry->setCached(true);   // TODO fix this so new entries go to memory if the cache is not full map, little bit o cheatin here
    }

    /* Put request in MSHR (all GetS requests need to be buffered while they wait for data) and stall if there are waiting requests ahead of us */
    if (!(mshr->elementIsHit(ev->getBaseAddr(), ev))) {
        bool conflict = mshr->isHit(ev->getBaseAddr());
        if (!mshr->insert(ev->getBaseAddr(), ev)) {
            mshrNACKRequest(ev);
            return;
        }
        if (conflict) {
            return; // stall event until conflicting request finishes
        } else {
            profileRequestRecv(ev, entry);
        }
    }

    if (!entry->isCached()) {
        dbg.debug(_L6_, "Entry %" PRIx64 " not in cache.  Requesting from memory.\n", entry->getBaseAddr());
        getDirEntryFromMemory(entry);
        return;
    }

    State state = entry->getState();
    switch (state) {
        case I:
            entry->setState(IM);
            issueMemoryRequest(ev, entry);
            break;
        case S:
            if (entry->getSharerCount() == 1 && entry->isSharer(node_id(ev->getSrc()))) {
                entry->setState(SM);
                issueMemoryRequest(ev, entry);
            } else {
                entry->setState(S_Inv);
                issueInvalidates(ev, entry);
            }
            break;
        case M:
            entry->setState(M_Inv);
            issueFetch(ev, entry, FetchInv);
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "Directory %s received %s but state is %s\n", getName().c_str(), CommandString[ev->getCmd()], BccLineString[state]);
    }
}

void DirectoryController::issueInvalidates(MemEvent * ev, DirEntry * entry) {
    for (uint32_t i = 0; i < numTargets; i++) {
        if (i == node_name_to_id(ev->getSrc())) continue;
        if (entry->isSharer(i)) {
            sendInvalidate(i, ev, entry);
            entry->incrementWaitingAcks();
        }
    }
    entry->lastRequest = DirEntry::NO_LAST_REQUEST;
    dbg.debug(_L4_, "Sending Invalidates to fulfill request for exclusive, BsAddr = %" PRIx64 ".\n", entry->getBaseAddr());
}

/* Send Fetch to owner */
void DirectoryController::issueFetch(MemEvent * ev, DirEntry * entry, Command cmd) {
    MemEvent * fetch = new MemEvent(this, ev->getAddr(), ev->getBaseAddr(), cmd, cacheLineSize);
    fetch->setDst(nodeid_to_name[entry->getOwner()]);
    entry->lastRequest = fetch->getID();
    profileRequestSent(fetch);
    sendEventToCaches(fetch, timestamp + accessLatency);
}

/* Send Get* request to memory */
void DirectoryController::issueMemoryRequest(MemEvent * ev, DirEntry * entry) {
    Addr localAddr          = convertAddressToLocalAddress(ev->getAddr());
    Addr localBaseAddr      = convertAddressToLocalAddress(ev->getBaseAddr());
    MemEvent *reqEv         = new MemEvent(this, localAddr, localBaseAddr, ev->getCmd(), cacheLineSize);
    memReqs[reqEv->getID()] = ev->getBaseAddr();
    entry->lastRequest      = reqEv->getID();
    profileRequestSent(reqEv);
    
    uint64_t deliveryTime = timestamp + accessLatency;

    if (directMemoryLink) {
        memMsgQueue.insert(std::pair<uint64_t,MemEvent*>(deliveryTime, reqEv));
    } else {
        reqEv->setDst(memoryName);
        netMsgQueue.insert(std::pair<uint64_t,MemEvent*>(deliveryTime, reqEv));
    }
    dbg.debug(_L3_, "SEND: %s \tMemReq. Cmd = %s, BaseAddr = 0x%" PRIx64 ",  Time = %" PRIu64 "\n", getName().c_str(), CommandString[reqEv->getCmd()], entry->getBaseAddr(), getCurrentSimTimeNano());
    dbg.debug(_L5_, "Requesting data from memory.  Cmd = %s, BaseAddr = x%" PRIx64 ", Size = %u, noncacheable = %s\n", CommandString[reqEv->getCmd()], reqEv->getBaseAddr(), reqEv->getSize(), reqEv->queryFlag(MemEvent::F_NONCACHEABLE) ? "true" : "false");
}

/* handle NACK */
void DirectoryController::handleNACK(MemEvent * ev) {
    MemEvent* origEvent = ev->getNACKedEvent();
    profileResponseRecv(ev);
    
    DirEntry *entry = getDirEntry(origEvent->getBaseAddr());
    dbg.debug(_L5_, "Orig resp ID = (%" PRIu64 ",%d), Nack resp ID = (%" PRIu64 ",%d), last req ID = (%" PRIu64 ",%d)\n", 
	origEvent->getResponseToID().first, origEvent->getResponseToID().second, ev->getResponseToID().first, 
	ev->getResponseToID().second, entry->lastRequest.first, entry->lastRequest.second);
    
    /* Retry request if it has not already been handled */
    if ((ev->getResponseToID() == entry->lastRequest) || origEvent->getCmd() == Inv) {
	/* Re-send request */
	sendEventToCaches(origEvent, timestamp + mshrLatency);
	dbg.debug(_L5_,"Orig Cmd NACKed, retry = %s \n", CommandString[origEvent->getCmd()]);
    } else {
	dbg.debug(_L5_,"Orig Cmd NACKed, no retry = %s \n", CommandString[origEvent->getCmd()]);
    }
    
    delete ev;
    return;
}


/* 
 * Handle PutS request - either a request or a response to an Inv 
 * Important: PutS are handled regardless of whether entry is "cached" because 
 * the lack of AckPut messages in the protocol leads to races if we stall or NACK a PutS
 * TODO fix by adding AckPut messages
 */
void DirectoryController::handlePutS(MemEvent * ev) {
    DirEntry * entry = getDirEntry(ev->getBaseAddr());
    if (!entry) {
        dbg.fatal(CALL_INFO, -1, "%s, Error: Directory entry does not exist. Addr = 0x%" PRIx64 ", Cmd = PutS, Src = %s. Time = %" PRIu64 "ns\n", 
                getName().c_str(), ev->getBaseAddr(), ev->getSrc().c_str(), getCurrentSimTimeNano());
    }


    entry->removeSharer(node_name_to_id(ev->getSrc()));
    if (mshr->elementIsHit(ev->getBaseAddr(), ev)) mshr->removeElement(ev->getBaseAddr(), ev);
    
    State state = entry->getState();
    Addr addr = entry->getBaseAddr();
    switch (state) {
        case S:
            profileRequestRecv(ev, entry);
            if (entry->getSharerCount() == 0) {
                entry->setState(I);
            }
            postRequestProcessing(ev, entry);   // profile & delete ev
            updateCache(entry);             // update dir cache
            break;
        case S_D:
            profileRequestRecv(ev, entry);
            postRequestProcessing(ev, entry);
            break;
        case S_Inv:
            profileResponseRecv(ev);
            entry->decrementWaitingAcks();
            delete ev;
            if (entry->getWaitingAcks() == 0) {
                entry->setState(I);
                replayWaitingEvents(addr);
            }
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "%s, Error: Directory received PutS but state is %s. Addr = 0x%" PRIx64 ", Src = %s. Time = %" PRIu64 "ns\n",
                    getName().c_str(), BccLineString[state], ev->getBaseAddr(), ev->getSrc().c_str(), getCurrentSimTimeNano());
    }
}

void DirectoryController::handlePutX(MemEvent * ev) {
    DirEntry * entry = getDirEntry(ev->getBaseAddr());
    
    /* Error checking */
    if (!entry) {
        dbg.fatal(CALL_INFO, -1, "%s, Error: Directory entry does not exist. Addr = 0x%" PRIx64 ", Cmd = %s, Src = %s. Time = %" PRIu64 "ns\n", 
                getName().c_str(), ev->getBaseAddr(), CommandString[ev->getCmd()], ev->getSrc().c_str(), getCurrentSimTimeNano());
    }
    if (!((uint32_t)entry->getOwner() == node_name_to_id(ev->getSrc()))) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: received PutM from a node who does not own the block. Addr = 0x%" PRIx64 ", Cmd = %s, Src = %s. Time = %" PRIu64 "ns\n",
                getName().c_str(), ev->getBaseAddr(), CommandString[ev->getCmd()], ev->getSrc().c_str(), getCurrentSimTimeNano());
    }

    /* Get entry from memory if needed */
    if (!(entry->isCached())) {
        if (!(mshr->elementIsHit(ev->getBaseAddr(),ev))) {
            mshrHits++;
            bool inserted = mshr->insert(ev->getBaseAddr(),ev);
            dbg.debug(_L8_, "Inserting request in mshr. Cmd = %s, BaseAddr = 0x%" PRIx64 ", Addr = 0x%" PRIx64 ", MSHR size: %d\n", CommandString[ev->getCmd()], ev->getBaseAddr(), ev->getAddr(), mshr->getSize());
            if (!inserted) {
                dbg.debug(_L8_, "MSHR is full. NACKing request\n");
                mshrNACKRequest(ev);
                return;
            }
        }
        dbg.debug(_L6_, "Entry %" PRIx64 " not in cache.  Requesting from memory.\n", entry->getBaseAddr());
        getDirEntryFromMemory(entry);
        return;
    } 

    /* Handle request */
    State state = entry->getState();
    switch  (state) {
        case M:
            profileRequestRecv(ev, entry);
            writebackData(ev);
            entry->clearOwner();
            entry->addSharer(node_name_to_id(ev->getSrc()));
            entry->setState(S);
            postRequestProcessing(ev, entry);   // profile & delete ev
            updateCache(entry);             // update cache
            break;
        //case M_Inv:
        case M_InvX:
            handleFetchXResp(ev);
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "%s, Error: Directory received PutM but state is %s. Addr = 0x%" PRIx64 ", Src = %s. Time = %" PRIu64 "ns\n",
                    getName().c_str(), BccLineString[state], ev->getBaseAddr(), ev->getSrc().c_str(), getCurrentSimTimeNano());
    }
}

void DirectoryController::handlePutE(MemEvent * ev) {
    DirEntry * entry = getDirEntry(ev->getBaseAddr());
    /* Error checking */
    if (!entry) {
        dbg.fatal(CALL_INFO, -1, "%s, Error: Directory entry does not exist. Addr = 0x%" PRIx64 ", Cmd = %s, Src = %s. Time = %" PRIu64 "ns\n", 
                getName().c_str(), ev->getBaseAddr(), CommandString[ev->getCmd()], ev->getSrc().c_str(), getCurrentSimTimeNano());
    }
    if (!((uint32_t)entry->getOwner() == node_name_to_id(ev->getSrc()))) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: received PutM from a node who does not own the block. Addr = 0x%" PRIx64 ", Cmd = %s, Src = %s. Time = %" PRIu64 "ns\n",
                getName().c_str(), ev->getBaseAddr(), CommandString[ev->getCmd()], ev->getSrc().c_str(), getCurrentSimTimeNano());
    }

    if (!(entry->isCached())) {
        if (!(mshr->elementIsHit(ev->getBaseAddr(),ev))) {
            mshrHits++;
            bool inserted = mshr->insert(ev->getBaseAddr(),ev);
            dbg.debug(_L8_, "Inserting request in mshr. Cmd = %s, BaseAddr = 0x%" PRIx64 ", Addr = 0x%" PRIx64 ", MSHR size: %d\n", CommandString[ev->getCmd()], ev->getBaseAddr(), ev->getAddr(), mshr->getSize());
            if (!inserted) {
                dbg.debug(_L8_, "MSHR is full. NACKing request\n");
                mshrNACKRequest(ev);
                return;
            }
        }
        dbg.debug(_L6_, "Entry %" PRIx64 " not in cache.  Requesting from memory.\n", entry->getBaseAddr());
        getDirEntryFromMemory(entry);
        return;
    } 

    /* Update owner state */
    profileRequestRecv(ev, entry);
    entry->clearOwner();

    State state = entry->getState();
    switch  (state) {
        case M:
            entry->setState(I);
            postRequestProcessing(ev, entry);  // profile & delete ev
            updateCache(entry);         // update cache;
            break;
        case M_Inv:     /* If PutE comes with data then we can handle this immediately but otherwise */
            entry->setState(IM);
            issueMemoryRequest(mshr->lookupFront(ev->getBaseAddr()), entry);
            postRequestProcessing(ev, entry);  // profile & delete ev
            break;
        case M_InvX:
            entry->setState(IS);
            issueMemoryRequest(mshr->lookupFront(ev->getBaseAddr()), entry);
            postRequestProcessing(ev, entry);  // profile & delete ev
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "%s, Error: Directory received PutM but state is %s. Addr = 0x%" PRIx64 ", Src = %s. Time = %" PRIu64 "ns\n",
                    getName().c_str(), BccLineString[state], ev->getBaseAddr(), ev->getSrc().c_str(), getCurrentSimTimeNano());
    }
}

void DirectoryController::handlePutM(MemEvent * ev) {
    DirEntry * entry = getDirEntry(ev->getBaseAddr());
    /* Error checking */
    if (!entry) {
        dbg.fatal(CALL_INFO, -1, "%s, Error: Directory entry does not exist. Addr = 0x%" PRIx64 ", Cmd = %s, Src = %s. Time = %" PRIu64 "ns\n", 
                getName().c_str(), ev->getBaseAddr(), CommandString[ev->getCmd()], ev->getSrc().c_str(), getCurrentSimTimeNano());
    }
    if (!((uint32_t)entry->getOwner() == node_name_to_id(ev->getSrc()))) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: received PutM from a node who does not own the block. Addr = 0x%" PRIx64 ", Cmd = %s, Src = %s. Time = %" PRIu64 "ns\n",
                getName().c_str(), ev->getBaseAddr(), CommandString[ev->getCmd()], ev->getSrc().c_str(), getCurrentSimTimeNano());
    }

    if (!(entry->isCached())) {
        if (!(mshr->elementIsHit(ev->getBaseAddr(),ev))) {
            mshrHits++;
            bool inserted = mshr->insert(ev->getBaseAddr(),ev);
            dbg.debug(_L8_, "Inserting request in mshr. Cmd = %s, BaseAddr = 0x%" PRIx64 ", Addr = 0x%" PRIx64 ", MSHR size: %d\n", CommandString[ev->getCmd()], ev->getBaseAddr(), ev->getAddr(), mshr->getSize());
            if (!inserted) {
                dbg.debug(_L8_, "MSHR is full. NACKing request\n");
                mshrNACKRequest(ev);
                return;
            }
        }
        dbg.debug(_L6_, "Entry %" PRIx64 " not in cache.  Requesting from memory.\n", entry->getBaseAddr());
        getDirEntryFromMemory(entry);
        return;
    } 

    State state = entry->getState();
    switch  (state) {
        case M:
            profileRequestRecv(ev, entry);
            writebackData(ev);
            entry->clearOwner();
            entry->setState(I);
            postRequestProcessing(ev, entry);  // profile & delete event
            updateCache(entry);
            break;
        case M_Inv:
        case M_InvX:
            handleFetchResp(ev);
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "%s, Error: Directory received PutM but state is %s. Addr = 0x%" PRIx64 ", Src = %s. Time = %" PRIu64 "ns, %" PRIu64 " cycles.\n",
                    getName().c_str(), BccLineString[state], ev->getBaseAddr(), ev->getSrc().c_str(), getCurrentSimTimeNano(), timestamp);
    }
}

/* Handle the incoming event as a fetch Response (FetchResp, FetchXResp, PutM, PutX) */
void DirectoryController::handleFetchResp(MemEvent * ev) {
    dbg.debug(_L4_, "Finishing Fetch.\n");
    
    DirEntry * entry = getDirEntry(ev->getBaseAddr());
    MemEvent * reqEv = mshr->removeFront(ev->getBaseAddr());
    
    /* Error checking */
    if (!entry) {
        dbg.fatal(CALL_INFO, -1, "%s, Error: Directory entry does not exist. Addr = 0x%" PRIx64 ", Cmd = %s, Src = %s. Time = %" PRIu64 "ns, %" PRIu64 " cycles\n", 
                getName().c_str(), ev->getBaseAddr(), CommandString[ev->getCmd()], ev->getSrc().c_str(), getCurrentSimTimeNano(), timestamp);
    }
    if (!((uint32_t)entry->getOwner() == node_name_to_id(ev->getSrc()))) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: received FetchResp from a node who does not own the block. Addr = 0x%" PRIx64 ", Cmd = %s, Src = %s. Time = %" PRIu64 "ns, %" PRIu64 " cycles\n",
                getName().c_str(), ev->getBaseAddr(), CommandString[ev->getCmd()], ev->getSrc().c_str(), getCurrentSimTimeNano(), timestamp);
    }
   
    /* Profile response */
    profileResponseRecv(ev);

    /* Clear previous owner state and writeback block. */
    entry->clearOwner();
    if (reqEv->getCmd() != GetX) {  // GetX will be dirty anyways so don't bother with writeback
        writebackData(ev);
    }

    MemEvent * respEv;
    State state = entry->getState(); 
    
    /* Handle request */
    switch (state) {
        case M_Inv:     // GetX request
            entry->setOwner(node_id(reqEv->getSrc()));
            respEv = reqEv->makeResponse();
            entry->setState(M);
            break;
        case M_InvX:    // GetS request
            if (protocol == "MESI" && entry->getSharerCount() == 0) {
                entry->setOwner(node_id(reqEv->getSrc()));
                respEv = reqEv->makeResponse(E);
                entry->setState(M);
            } else {
                entry->addSharer(node_id(reqEv->getSrc()));
                respEv = reqEv->makeResponse();
                entry->setState(S);
            }
            break;
        default:
            dbg.fatal(CALL_INFO, -1, "%s, Error: Directory received %s but state is %s. Addr = 0x%" PRIx64 ", Src = %s. Time = %" PRIu64 "ns, %" PRIu64 " cycles\n",
                    getName().c_str(), CommandString[ev->getCmd()], BccLineString[state], ev->getBaseAddr(), ev->getSrc().c_str(), getCurrentSimTimeNano(), timestamp);
    }   
    respEv->setPayload(ev->getPayload());
    profileResponseSent(respEv);
    sendEventToCaches(respEv, timestamp + mshrLatency);
    
    delete ev;
    
    postRequestProcessing(reqEv, entry);
    replayWaitingEvents(entry->getBaseAddr());
    updateCache(entry);
}

void DirectoryController::handleFetchXResp(MemEvent * ev) {
    dbg.debug(_L4_, "Finishing Fetch.\n");
    
    DirEntry * entry = getDirEntry(ev->getBaseAddr());
    MemEvent * reqEv = mshr->removeFront(ev->getBaseAddr());
    
    /* Error checking */
    if (!entry) {
        dbg.fatal(CALL_INFO, -1, "%s, Error: Directory entry does not exist. Addr = 0x%" PRIx64 ", Cmd = %s, Src = %s. Time = %" PRIu64 "ns, %" PRIu64 " cycles\n", 
                getName().c_str(), ev->getBaseAddr(), CommandString[ev->getCmd()], ev->getSrc().c_str(), getCurrentSimTimeNano(), timestamp);
    }
    if (!((uint32_t)entry->getOwner() == node_name_to_id(ev->getSrc()))) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: received FetchResp from a node who does not own the block. Addr = 0x%" PRIx64 ", Cmd = %s, Src = %s. Time = %" PRIu64 "ns, %" PRIu64 " cycles\n",
                getName().c_str(), ev->getBaseAddr(), CommandString[ev->getCmd()], ev->getSrc().c_str(), getCurrentSimTimeNano(), timestamp );
    }
   
    /* Profile response */
    profileResponseRecv(ev);

    /* Clear previous owner state and writeback block. */
    entry->clearOwner();
    entry->addSharer(node_name_to_id(ev->getSrc()));
    entry->setState(S);
    writebackData(ev);

    MemEvent * respEv = reqEv->makeResponse(); 
    entry->addSharer(node_id(reqEv->getSrc()));
    
    respEv->setPayload(ev->getPayload());
    profileResponseSent(respEv);
    sendEventToCaches(respEv, timestamp + mshrLatency);
    
    delete ev;

    postRequestProcessing(reqEv, entry);
    replayWaitingEvents(entry->getBaseAddr());
    updateCache(entry);

}

/* Handle GetSResp or GetXResp from memory */
void DirectoryController::handleDataResponse(MemEvent * ev) {
    DirEntry * entry = getDirEntry(ev->getBaseAddr());
    
    State state = entry->getState();

    MemEvent * reqEv = mshr->removeFront(ev->getBaseAddr());
    dbg.debug(_L10_, "\t%s\tMSHR remove event <%s, %" PRIx64 ">\n", getName().c_str(), CommandString[reqEv->getCmd()], reqEv->getBaseAddr());
    //dbg.debug(_L9_, "\t%s\tHandling stalled event: %s, %s\n", CommandString[reqEv->getCmd()], reqEv->getSrc().c_str());

    MemEvent * respEv;
    switch (state) {
        case IS:
        case S_D:
            if (protocol == "MESI" && entry->getSharerCount() == 0) {
                respEv = reqEv->makeResponse(E);
                entry->setState(M);
                entry->setOwner(node_id(reqEv->getSrc()));
            } else {
                respEv = reqEv->makeResponse();
                entry->setState(S);
                entry->addSharer(node_id(reqEv->getSrc()));
            }
            break;
        case IM:
        case SM:
            respEv = reqEv->makeResponse();
            entry->setState(M);
            entry->setOwner(node_id(reqEv->getSrc()));
            entry->clearSharers();  // Case SM: new owner was a sharer
            break;
        default:
            dbg.fatal(CALL_INFO,1,"Directory %s received Get Response for addr 0x%" PRIx64 " but state is %s\n", getName().c_str(), ev->getBaseAddr(), BccLineString[state]);
    }

    respEv->setSize(cacheLineSize);
    respEv->setPayload(ev->getPayload());
    profileResponseSent(respEv);
    sendEventToCaches(respEv, timestamp + mshrLatency);
    
    dbg.debug(_L4_, "Sending requested data for 0x%" PRIx64 " to %s, granted state = %s\n", entry->getBaseAddr(), respEv->getDst().c_str(), BccLineString[respEv->getGrantedState()]);
    
    postRequestProcessing(reqEv, entry);
    replayWaitingEvents(entry->getBaseAddr());
    updateCache(entry);
}

void DirectoryController::handleMemoryResponse(SST::Event *event){
    MemEvent *ev = static_cast<MemEvent*>(event);
    
    dbg.debug(_L3_, "\n\n----------------------------------------------------------------------------------------\n");
    
    if (ev->queryFlag(MemEvent::F_NONCACHEABLE)) {
        if (noncacheMemReqs.find(ev->getResponseToID()) != noncacheMemReqs.end()) {
            Addr globalBaseAddr = noncacheMemReqs[ev->getID()].first;
            Addr globalAddr = noncacheMemReqs[ev->getID()].second;
            dbg.debug(_L3_, "EVENT: %s, Received: MemResp: Cmd = %s, BaseAddr = 0x%" PRIx64 ", Size = %u, Time = %" PRIu64 "\n", 
                    getName().c_str(), CommandString[ev->getCmd()], globalBaseAddr, ev->getSize(),getCurrentSimTimeNano());
            ev->setBaseAddr(globalBaseAddr);
            ev->setAddr(globalAddr);
            noncacheMemReqs.erase(ev->getResponseToID());
            profileResponseSent(ev);
            
            network->send(ev);
            return;
        }
        dbg.fatal(CALL_INFO, -1, "%s, Error: Received unexpected noncacheable response from memory. Addr = 0x%" PRIx64 ", Cmd = %s. Time = %" PRIu64 "ns\n", 
                getName().c_str(), ev->getBaseAddr(), CommandString[ev->getCmd()], getCurrentSimTimeNano());
    }


    if (ev->getBaseAddr() == 0 && dirEntryMiss.find(ev->getResponseToID()) != dirEntryMiss.end()) {    // directory entry miss
        dbg.debug(_L3_, "EVENT: %s, Received DirEntry: BaseAddr = 0x%" PRIx64 ", Src: Memory, Size = %u, Time = %" PRIu64 "\n", 
                getName().c_str(), ev->getBaseAddr(), ev->getSize(),getCurrentSimTimeNano());
        ev->setBaseAddr(dirEntryMiss[ev->getResponseToID()]);
        dirEntryMiss.erase(ev->getResponseToID());
        handleDirEntryMemoryResponse(ev);
    } else if (memReqs.find(ev->getResponseToID()) != memReqs.end()){
        ev->setBaseAddr(memReqs[ev->getResponseToID()]);
        Addr targetBlock = memReqs[ev->getResponseToID()];
        dbg.debug(_L3_, "RECV: %s, MemResp: Cmd = %s, BaseAddr = 0x%" PRIx64 ", Size = %u, Time = %" PRIu64 "\n", getName().c_str(), CommandString[ev->getCmd()], targetBlock, ev->getSize(),getCurrentSimTimeNano());
        memReqs.erase(ev->getResponseToID());
        handleDataResponse(ev);

    } else {
        dbg.fatal(CALL_INFO, -1, "%s, Error: Received unexpected response from memory - matching request not found. Addr = 0x%" PRIx64 ", Cmd = %s. Time = %" PRIu64 "ns\n",
                getName().c_str(), ev->getBaseAddr(), CommandString[ev->getCmd()], getCurrentSimTimeNano());
    }

    delete ev;
}

/* Handle response for directory entry */
void DirectoryController::handleDirEntryMemoryResponse(MemEvent * ev) {
    Addr dirAddr = ev->getBaseAddr();

    DirEntry * entry = getDirEntry(dirAddr);
    entry->setCached(true);

    State st = entry->getState();
    switch (st) {
        case I_d:
            entry->setState(I);
            break;
        case S_d:
            entry->setState(S);
        case M_d:
            entry->setState(M);
        default:
            dbg.fatal(CALL_INFO, -1, "Directory Controller %s: DirEntry response received for addr 0x%" PRIx64 " but state is %s\n", getName().c_str(), entry->getBaseAddr(), BccLineString[st]);
    }
    MemEvent * reqEv = mshr->lookupFront(dirAddr);
    processPacket(reqEv);
}

/* Send request for directory entry to memory */
void DirectoryController::getDirEntryFromMemory(DirEntry * entry) {
    State st = entry->getState();
    switch (st) {
        case I:
            entry->setState(I_d);
            break;
        case S:
            entry->setState(S_d);
        case M:
            entry->setState(M_d);
        default:
            dbg.fatal(CALL_INFO,-1,"Direcctory Controller %s: cache miss for addr 0x%" PRIx64 " but state is %s\n",getName().c_str(),entry->getBaseAddr(), BccLineString[st]);
    }
    
    Addr entryAddr       = 0; /* Dummy addr reused for dir cache misses */
    MemEvent *me         = new MemEvent(this, entryAddr, entryAddr, GetS, cacheLineSize);
    me->setSize(entrySize);
    dirEntryMiss[me->getID()] = entry->getBaseAddr();
    profileRequestSent(me);
    
    uint64_t deliveryTime = timestamp + accessLatency;
    if (directMemoryLink) {
        memMsgQueue.insert(std::pair<uint64_t,MemEvent*>(deliveryTime, me));
    } else {
        me->setDst(memoryName);
        netMsgQueue.insert(std::pair<uint64_t,MemEvent*>(deliveryTime, me));
    }
    dbg.debug(_L10_, "Requesting Entry from memory for 0x%" PRIx64 "(%" PRIu64 ", %d)\n", entry->getBaseAddr(), me->getID().first, me->getID().second);
}



void DirectoryController::mshrNACKRequest(MemEvent* ev) {
    MemEvent * nackEv = ev->makeNACKResponse(this,ev);
    profileResponseSent(nackEv);
    sendEventToCaches(nackEv, timestamp + 1);
}

void DirectoryController::printStatus(Output &out){
    out.output("MemHierarchy::DirectoryController %s\n", getName().c_str());
    out.output("\t# Entries in cache:  %zu\n", entryCacheSize);
    out.output("\t# Requests in queue:  %zu\n", workQueue.size());
    for(std::list<MemEvent*>::iterator i = workQueue.begin() ; i != workQueue.end() ; ++i){
        out.output("\t\t(%" PRIu64 ", %d)\n", (*i)->getID().first, (*i)->getID().second);
    }
}



DirectoryController::DirEntry* DirectoryController::getDirEntry(Addr baseAddr){
	std::map<Addr, DirEntry*>::iterator i = directory.find(baseAddr);
	if(directory.end() == i) return NULL;
	return i->second;
}



DirectoryController::DirEntry* DirectoryController::createDirEntry(Addr baseAddr, Addr addr, uint32_t reqSize){
    dbg.debug(_L10_, "Creating Directory Entry for 0x%" PRIx64 "\n", baseAddr);
    DirEntry *entry = new DirEntry(baseAddr, addr, numTargets, &dbg);
    entry->cacheIter = entryCache.end();
    directory[baseAddr] = entry;
    return entry;
}


void DirectoryController::sendInvalidate(int target, MemEvent * reqEv, DirEntry* entry){
    MemEvent *me = new MemEvent(this, entry->getBaseAddr(), entry->getBaseAddr(), Inv, cacheLineSize);
    me->setDst(nodeid_to_name[target]);
    me->setAckNeeded();
    me->setRqstr(reqEv->getRqstr());
    dbg.debug(_L4_, "Sending Invalidate.  Dst: %s\n", nodeid_to_name[target].c_str());
    profileRequestSent(me);
    
    uint64_t deliveryTime = timestamp + accessLatency;
    netMsgQueue.insert(std::pair<uint64_t,MemEvent*>(deliveryTime, me));
    
    dbg.debug(_L3_, "SEND: %s \tCmd = %s, BaseAddr = 0x%" PRIx64 ",  Dst = %s, Time = %" PRIu64 "\n", getName().c_str(), CommandString[me->getCmd()], me->getBaseAddr(), me->getDst().c_str(), getCurrentSimTimeNano());
}


uint32_t DirectoryController::node_id(const std::string &name){
	uint32_t id;
	std::map<std::string, uint32_t>::iterator i = node_lookup.find(name);
	if(node_lookup.end() == i){
		node_lookup[name] = id = targetCount++;
        nodeid_to_name.resize(targetCount);
        nodeid_to_name[id] = name;
	}
    else id = i->second;
    
	return id;
}



uint32_t DirectoryController::node_name_to_id(const std::string &name){
    std::map<std::string, uint32_t>::iterator i = node_lookup.find(name);

    if(node_lookup.end() == i) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: Attempt to lookup node ID but name not found: %s. Time = %" PRIu64 "ns\n", 
                getName().c_str(), name.c_str(), getCurrentSimTimeNano());
    }

    uint32_t id = i->second;
    return id;
}

void DirectoryController::updateCache(DirEntry *entry){
    if(0 == entryCacheMaxSize){
        sendEntryToMemory(entry);
    } else {
        /* Find if we're in the cache */
        if(entry->cacheIter != entryCache.end()){
            entryCache.erase(entry->cacheIter);
            --entryCacheSize;
            entry->cacheIter = entryCache.end();
        }

        /* Find out if we're no longer cached, and just remove */
        if (entry->getState() == I){
            dbg.debug(_L10_, "Entry for 0x%" PRIx64 " has no references - purging\n", entry->getBaseAddr());
            directory.erase(entry->getBaseAddr());
            delete entry;
            return;
        } else {
            entryCache.push_front(entry);
            entry->cacheIter = entryCache.begin();
            ++entryCacheSize;

            while(entryCacheSize > entryCacheMaxSize){
                DirEntry *oldEntry = entryCache.back();
                // If the oldest entry is still in progress, everything is in progress
                if(mshr->isHit(oldEntry->getBaseAddr())) break;

                dbg.debug(_L10_, "entryCache too large.  Evicting entry for 0x%" PRIx64 "\n", oldEntry->getBaseAddr());
                entryCache.pop_back();
                --entryCacheSize;
                oldEntry->cacheIter = entryCache.end();
                oldEntry->setCached(false);
                sendEntryToMemory(oldEntry);
            }
        }
    }
}



void DirectoryController::sendEntryToMemory(DirEntry *entry){
    Addr entryAddr = 0; // Always use local address 0 for directory entries
    MemEvent *me   = new MemEvent(this, entryAddr, entryAddr, PutE, cacheLineSize); // MemController discards PutE's without writeback so this is safe
    me->setSize(entrySize);
    profileRequestSent(me);

    uint64_t deliveryTime = timestamp + accessLatency;

    if (directMemoryLink) {
        memMsgQueue.insert(std::pair<uint64_t,MemEvent*>(deliveryTime, me));
    } else {
        me->setDst(memoryName);
        netMsgQueue.insert(std::pair<uint64_t,MemEvent*>(deliveryTime, me));
    }
}



MemEvent::id_type DirectoryController::writebackData(MemEvent *data_event){
    Addr localBaseAddr = convertAddressToLocalAddress(data_event->getBaseAddr());
    MemEvent *ev       = new MemEvent(this, localBaseAddr, localBaseAddr, PutM, cacheLineSize);

    if(data_event->getPayload().size() != cacheLineSize) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: Writing back data request but payload does not match cache line size of %uB. Addr = 0x%" PRIx64 ", Cmd = %s, Src = %s, Size = %zu. Time = %" PRIu64 "ns\n",
                getName().c_str(), cacheLineSize, ev->getBaseAddr(), CommandString[ev->getCmd()], ev->getSrc().c_str(), ev->getPayload().size(), getCurrentSimTimeNano());
    }

    ev->setSize(data_event->getPayload().size());
    ev->setPayload(data_event->getPayload());
    profileRequestSent(ev);
    
    uint64_t deliveryTime = timestamp + accessLatency;
    if (directMemoryLink) {
        memMsgQueue.insert(std::pair<uint64_t,MemEvent*>(deliveryTime, ev));
    } else {
        ev->setDst(memoryName);
        netMsgQueue.insert(std::pair<uint64_t,MemEvent*>(deliveryTime, ev));
    }
    dbg.debug(_L3_, "SEND: %s \tMemReq. Cmd = %s, BaseAddr = 0x%" PRIx64 ",  Time = %" PRIu64 "\n", getName().c_str(), CommandString[ev->getCmd()], data_event->getBaseAddr(), getCurrentSimTimeNano());
    
    dbg.debug(_L5_, "Writing back data. Cmd = %s, BaseAddr = 0x%" PRIx64 ", Size = %u\n", CommandString[ev->getCmd()], ev->getBaseAddr(), ev->getSize());
    return ev->getID();
}

void DirectoryController::postRequestProcessing(MemEvent * ev, DirEntry * entry) {
    ++numReqsProcessed;
    totalReqProcessTime += (getCurrentSimTimeNano() - ev->getDeliveryTime());
    Command cmd = ev->getCmd();
    if (cmd == GetS || cmd == GetX || cmd == GetSEx) {
        totalGetReqProcessTime += (getCurrentSimTimeNano() - ev->getDeliveryTime());
    } else {
        totalReplProcessTime += (getCurrentSimTimeNano() - ev->getDeliveryTime() + 1);
    }

    delete ev;
    entry->setToSteadyState();
}

void DirectoryController::replayWaitingEvents(Addr addr) {
    if (mshr->isHit(addr)) {
        vector<mshrType> replayEntries = mshr->removeAll(addr);
        for (vector<mshrType>::reverse_iterator it = replayEntries.rbegin(); it != replayEntries.rend(); it++) {
            MemEvent *ev = boost::get<MemEvent*>((*it).elem);
            std::list<MemEvent*>::iterator insert_it = workQueue.begin();
            insert_it++;
            dbg.debug(_L5_, "Reactivating event %p. Cmd = %s, BaseAddr = 0x%" PRIx64 ", Addr = 0x%" PRIx64 ", MSHR size: %d\n", ev, CommandString[ev->getCmd()], ev->getBaseAddr(), ev->getAddr(), mshr->getSize());
            workQueue.insert(insert_it,ev);
        }
    }

}

void DirectoryController::sendEventToCaches(MemEvent *ev, uint64_t deliveryTime){
    dbg.debug(_L3_, "Sending Event. Cmd = %s, BaseAddr = 0x%" PRIx64 ", Dst = %s, Size = %u\n", CommandString[ev->getCmd()], ev->getBaseAddr(), ev->getDst().c_str(), ev->getSize());
    dbg.debug(_L3_, "SEND: %s \tCmd = %s, BaseAddr = 0x%" PRIx64 ",  Dst = %s, Time = %" PRIu64 "\n", getName().c_str(), CommandString[ev->getCmd()], ev->getBaseAddr(), ev->getDst().c_str(), getCurrentSimTimeNano());
    netMsgQueue.insert(std::pair<uint64_t,MemEvent*>(deliveryTime, ev));
}



bool DirectoryController::isRequestAddressValid(MemEvent *ev){
    Addr addr = ev->getBaseAddr();
    if(0 == interleaveSize){
        return(addr >= addrRangeStart && addr < addrRangeEnd);
    } else {
        if(addr < addrRangeStart) return false;
        if(addr >= addrRangeEnd) return false;

        addr        = addr - addrRangeStart;
        Addr offset = addr % interleaveStep;
        
        if(offset >= interleaveSize) return false;
        return true;
    }

}

Addr DirectoryController::convertAddressFromLocalAddress(Addr addr) {
    if(interleaveStep > interleaveSize) {
	dbg.fatal(CALL_INFO, -1, "%s, Error: in address conversion, interleaveStep (%" PRIu32 ") > interleaveSize (%" PRIu32 "). Addr = 0x%" PRIx64 ". Time = %" PRIu64 "\n",
		getName().c_str(), (uint32_t) interleaveStep, (uint32_t) interleaveSize, addr, getCurrentSimTimeNano());
    }

    Addr res = 0;
    if(0 == interleaveSize){
        res = addr + addrRangeStart;
    }
    else {
        Addr a 	    = addr;
        Addr step   = a / interleaveSize; 
        Addr offset = a % interleaveSize;
        res = (step * interleaveStep) + offset;
        res = res + addrRangeStart;

    }
    dbg.debug(_L10_, "Converted ACTUAL memory address 0x%" PRIx64 " to physical address 0x%" PRIx64 "\n", addr, res);
    return res;
}

Addr DirectoryController::convertAddressToLocalAddress(Addr addr){
    Addr res = 0;
    if(0 == interleaveSize){
        res = addr - addrRangeStart;
    }
    else {
        Addr a      = addr - addrRangeStart;
        Addr step   = a / interleaveStep;
        Addr offset = a % interleaveStep;
        res         = (step * interleaveSize) + offset;
    }
    dbg.debug(_L10_, "Converted physical address 0x%" PRIx64 " to ACTUAL memory address 0x%" PRIx64 "\n", addr, res);
    return res;
}



/*static char dirEntStatus[1024] = {0};
const char* DirectoryController::printDirectoryEntryStatus(Addr baseAddr){
    DirEntry *entry = getDirEntry(baseAddr);
    if(!entry){
        sprintf(dirEntStatus, "[Not Created]");
    } else {
        uint32_t refs = entry->countRefs();

        if(0 == refs) sprintf(dirEntStatus, "[Noncacheable]");
        else if(entry->isDirty()){
            uint32_t owner = entry->findOwner();
            sprintf(dirEntStatus, "[owned by %s]", nodeid_to_name[owner].c_str());
        }
        else sprintf(dirEntStatus, "[Shared by %u]", refs);


    }
    return dirEntStatus;
}*/



void DirectoryController::init(unsigned int phase){
    network->init(phase);

    if (!directMemoryLink && network->initDataReady() && !network->isValidDestination(memoryName)) {
        dbg.fatal(CALL_INFO,-1,"%s, Invalid param: net_memory_name - must name a valid memory component in the system. You specified: %s\n",getName().c_str(), memoryName.c_str());
    }
    /* Pass data on to memory */
    while(MemEvent *ev = network->recvInitData()){
        dbg.debug(_L10_, "Found Init Info for address 0x%" PRIx64 "\n", ev->getAddr());
        if(isRequestAddressValid(ev)){
            ev->setBaseAddr(convertAddressToLocalAddress(ev->getBaseAddr()));
            ev->setAddr(convertAddressToLocalAddress(ev->getAddr()));
            dbg.debug(_L10_, "Sending Init Data for address 0x%" PRIx64 " to memory\n", ev->getAddr());
            if (directMemoryLink) {
                memLink->sendInitData(ev);
            } else {
                ev->setDst(memoryName);
                network->sendInitData(ev);
            }
        }
        else delete ev;
    }

}



void DirectoryController::finish(void){
    network->finish();
    uint64_t getReq = GetSReqReceived + GetXReqReceived + GetSExReqReceived;
    uint64_t putReq = PutMReqReceived + PutEReqReceived + PutMReqReceived;
    Output out("", 0, 0, printStatsLoc);
    out.output("\n--------------------------------------------------------------------\n");
    out.output("--- Directory Controller\n");
    out.output("--- Name: %s\n", getName().c_str());
    out.output("--------------------------------------------------------------------\n");
    out.output("- Total requests received:  %" PRIu64 "\n", numReqsProcessed);
    out.output("- GetS received:            %" PRIu64 "\n", GetSReqReceived);
    out.output("- GetX received:            %" PRIu64 "\n", GetXReqReceived);
    out.output("- GetSEx received:          %" PRIu64 "\n", GetSExReqReceived);
    out.output("- PutM received:            %" PRIu64 "\n", PutMReqReceived);
    out.output("- PutE received:            %" PRIu64 "\n", PutEReqReceived);
    out.output("- PutS received:            %" PRIu64 "\n", PutSReqReceived);
    out.output("- NACK received:            %" PRIu64 "\n", NACKReceived);
    out.output("- FetchResp received:       %" PRIu64 "\n", FetchRespReceived);
    out.output("- FetchXResp received:      %" PRIu64 "\n", FetchRespXReceived);
    out.output("- PutM response received:   %" PRIu64 "\n", PutMRespReceived);
    out.output("- PutE response received:   %" PRIu64 "\n", PutERespReceived);
    out.output("- PutS response received:   %" PRIu64 "\n", PutSRespReceived);
    out.output("- Data reads issued:        %" PRIu64 "\n", dataReads);
    out.output("- Data writes issued:       %" PRIu64 "\n", dataWrites);
    out.output("- Dir entry reads:          %" PRIu64 "\n", dirEntryReads);
    out.output("- Dir entry writes:         %" PRIu64 "\n", dirEntryWrites);
    out.output("- Inv sent:                 %" PRIu64 "\n", InvSent);
    out.output("- FetchInv sent:            %" PRIu64 "\n", FetchInvSent);
    out.output("- FetchInvX sent:           %" PRIu64 "\n", FetchInvXSent);
    out.output("- GetSResp sent:            %" PRIu64 "\n", GetSRespSent);
    out.output("- GetXResp sent:            %" PRIu64 "\n", GetXRespSent);
    out.output("- NACKs sent:               %" PRIu64 "\n", NACKSent);
    out.output("- Avg Req Time:             %" PRIu64 " ns\n", (numReqsProcessed > 0) ? totalReqProcessTime / numReqsProcessed : 0);
    out.output("- Avg 'Get' Req Time:       %" PRIu64 " ns\n", (getReq > 0) ? totalGetReqProcessTime / getReq : 0);
    out.output("- Avg 'Put' Req Time:       %" PRIu64 " ns\n", (putReq > 0) ? totalReplProcessTime / putReq : 0);
    out.output("- Entry Cache Hits:         %" PRIu64 "\n", numCacheHits);
    out.output("- MSHR hits:                %" PRIu64 "\n", mshrHits);
}



void DirectoryController::setup(void){
    network->setup();

    const std::vector<MemNIC::PeerInfo_t> &ci = network->getPeerInfo();
    for(std::vector<MemNIC::PeerInfo_t>::const_iterator i = ci.begin() ; i != ci.end() ; ++i){
        dbg.debug(_L10_, "DC found peer %d(%s) of type %d.\n", i->first.network_addr, i->first.name.c_str(), i->first.type);
        if(MemNIC::TypeCache == i->first.type || MemNIC::TypeNetworkCache == i->first.type){
            numTargets++;
            if(blocksize) {
		if(blocksize != i->second.blocksize) {
			dbg.fatal(CALL_INFO, -1, "%s, Error: block size does not match blocksize=%" PRIu32 " != %" PRIu32 "\n",
				getName().c_str(), (uint32_t) blocksize, (uint32_t) i->second.blocksize);
		}
	    }
            else            blocksize = i->second.blocksize;
        }
    }
    if(0 == numTargets) dbg.fatal(CALL_INFO,-1,"%s, Error: Did not find any caches during init\n",getName().c_str());

    network->clearPeerInfo();
    entrySize = (numTargets+1)/8 +1;
}

