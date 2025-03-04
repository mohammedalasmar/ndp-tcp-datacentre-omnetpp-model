
#include "../../ndptransportlayer/Ndp/Ndp.h"

#include "../../ndptransportlayer/contract/ndp/NDPCommand_m.h"
#include "inet/networklayer/common/IPSocket.h"
#include "inet/networklayer/contract/INetworkProtocolControlInfo.h"
#include "inet/networklayer/common/IPProtocolId_m.h"
#include "inet/common/lifecycle/LifecycleOperation.h"
#include "inet/common/ModuleAccess.h"
#include "inet/common/lifecycle/NodeOperations.h"
#include "inet/common/lifecycle/NodeStatus.h"
#include "ndp_common/NDPSegment.h"
#include "NDPConnection.h"
#include "inet/ndp/ndptransportlayer/Ndp/queues/NDPMsgBasedSendQueue.h"

#ifdef WITH_IPv4
#include "inet/networklayer/ipv4/ICMPMessage_m.h"
#endif // ifdef WITH_IPv4

#ifdef WITH_IPv6
#include "inet/networklayer/icmpv6/ICMPv6Message_m.h"
#endif // ifdef WITH_IPv6

#define PACING_TIME 12  //    MTU/linkRate

#include "inet/ndp/application/ndpapp/NdpSinkApp.h"

#include "inet/networklayer/ipv4/IPv4RoutingTable.h"
#include "inet/networklayer/common/L3AddressResolver.h"

using namespace std;
namespace inet {

namespace ndp {

Define_Module(ndp::Ndp);
simsignal_t numRequestsRTOs = ndp::Ndp::registerSignal("numRequestsRTOs");

#define EPHEMERAL_PORTRANGE_START    1024
#define EPHEMERAL_PORTRANGE_END      5000

static std::ostream& operator<<(std::ostream& os, const Ndp::SockPair& sp) {
    os << "loc=" << sp.localAddr << ":" << sp.localPort << " " << "rem="
            << sp.remoteAddr << ":" << sp.remotePort;
    return os;
}

static std::ostream& operator<<(std::ostream& os, const Ndp::AppConnKey& app) {
    os << "connId=" << app.connId << " appGateIndex=" << app.appGateIndex;
    return os;
}

static std::ostream& operator<<(std::ostream& os, const NDPConnection& conn) {
    os << "connId=" << conn.connId << " "
            << NDPConnection::stateName(conn.getFsmState()) << " state={"
            << const_cast<NDPConnection&>(conn).getState()->info() << "}";
    return os;
}

void Ndp::initialize(int stage) {
    cSimpleModule::initialize(stage);
    if (stage == INITSTAGE_LOCAL) {
        const char *q;
        q = par("sendQueueClass");
        if (*q != '\0')
            throw cRuntimeError(
                    "Don't use obsolete sendQueueClass = \"%s\" parameter", q);

        q = par("receiveQueueClass");
        if (*q != '\0')
            throw cRuntimeError(
                    "Don't use obsolete receiveQueueClass = \"%s\" parameter",
                    q);

        lastEphemeralPort = EPHEMERAL_PORTRANGE_START;
        WATCH(lastEphemeralPort);

        WATCH_PTRMAP(ndpConnMap);
        WATCH_PTRMAP(ndpAppConnMap);

        requestTimerStamps.setName("requestTimerStamps");

        recordStatistics = par("recordStats");
        useDataNotification = par("useDataNotification");
    }
    else if (stage == INITSTAGE_TRANSPORT_LAYER) {
        requestTimerMsg = new cMessage("requestTimerMsg");
         requestTimerMsg->setContextPointer(this);

        cModule *host = findContainingNode(this);
        NodeStatus *nodeStatus = check_and_cast_nullable<NodeStatus *>(host ? host->getSubmodule("status") : nullptr);
        isOperational = (!nodeStatus) || nodeStatus->getState() == NodeStatus::UP;
        IPSocket ipSocket(gate("ipOut"));
        ipSocket.registerProtocol(IP_PROT_TCP);
    }
}



void Ndp::handleMessage(cMessage *msg) {

    if (!isOperational) {
        if (msg->isSelfMessage())
            throw cRuntimeError(
                    "Model error: self msg '%s' received when isOperational is false",
                    msg->getName());
        EV_ERROR << "NDP is turned off, dropping '" << msg->getName()
                        << "' message\n";
        delete msg;
    }

    else if (msg->isSelfMessage()) {
         if (msg == requestTimerMsg) {
            process_REQUEST_TIMER();
         }
        else{
        NDPConnection *conn = (NDPConnection *) msg->getContextPointer();
        bool ret = conn->processTimer(msg);
      //  if (!ret)
          //  removeConnection(conn);
        }

    }

////////// ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
////////// ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    else if (msg->arrivedOn("ipIn")) {
        if (false
#ifdef WITH_IPv4  || dynamic_cast<ICMPMessage *>(msg)
#endif // ifdef WITH_IPv4
#ifdef WITH_IPv6  || dynamic_cast<ICMPv6Message *>(msg)
#endif // ifdef WITH_IPv6
        ) {
            EV_DETAIL << "ICMP error received -- discarding\n"; // FIXME can ICMP packets really make it up to NDP???
            delete msg;
        } else {


            // must be an NDPSegment
            NDPSegment *ndpseg = check_and_cast<NDPSegment *>(msg);
            EV_INFO << " \n\n\n\n\n\n seg arrivard ..... pri111 =  " << ndpseg->getPriorityValue() << "\n\n\n";
            ndpseg->setPriorityValue(10);

            NDPSegment *ndpseg2 = check_and_cast<NDPSegment *>(msg);
            EV_INFO << " \n\n\n\n\n\n seg arrivard ..... pri22 =  " << ndpseg->getPriorityValue() << "\n\n\n";

            // get src/dest addresses
            L3Address srcAddr, destAddr;
            cObject *ctrl = ndpseg->removeControlInfo();
            if (!ctrl)  throw cRuntimeError("(%s)%s arrived without control info",  ndpseg->getClassName(), ndpseg->getName());

            INetworkProtocolControlInfo *controlInfo = check_and_cast<INetworkProtocolControlInfo *>(ctrl);
            srcAddr = controlInfo->getSourceAddress();
            destAddr = controlInfo->getDestinationAddress();
            //interfaceId = controlInfo->getInterfaceId();
            delete ctrl;


            NDPConnection *conn = nullptr;
            // process segment
             conn = findConnForSegment(ndpseg, srcAddr, destAddr);

            if (conn) {
                bool ret = conn->processNDPSegment(ndpseg, srcAddr, destAddr);
                if (!ret)
                    removeConnection(conn);
            } else {
                segmentArrivalWhileClosed(ndpseg, srcAddr, destAddr);
            }
        }
    }



////////// ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
////////// ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
////////////////must be from app /////////////////////////////////
    else {

        NDPCommand *controlInfo = check_and_cast<NDPCommand *>( msg->getControlInfo());
        int appGateIndex = msg->getArrivalGate()->getIndex();
        int connId = controlInfo->getConnId();
        EV_INFO << "\n\n\n ( appGateIndex , connId ) = ( " << appGateIndex << " , " << connId << " ) " << "\n\n\n";
        NDPConnection *conn = findConnForApp(appGateIndex, connId);
        if (!conn) {
            conn = createConnection(appGateIndex, connId);
            AppConnKey key;
            key.appGateIndex = appGateIndex;
            key.connId = connId;
            ndpAppConnMap[key] = conn;
            EV_INFO << "NDP connection created for " << msg << "\n";
        }
        bool ret = conn->processAppCommand(msg);
        if (!ret)
            removeConnection(conn);
    }

    if (hasGUI())
        updateDisplayString();
}





NDPConnection *Ndp::createConnection(int appGateIndex, int connId) {
    return new NDPConnection(this, appGateIndex, connId);
}

void Ndp::segmentArrivalWhileClosed(NDPSegment *ndpseg, L3Address srcAddr, L3Address destAddr) {
    NDPConnection *tmp = new NDPConnection();
    tmp->segmentArrivalWhileClosed(ndpseg, srcAddr, destAddr);
    delete tmp;
    delete ndpseg;
}

void Ndp::updateDisplayString() {
#if OMNETPP_VERSION < 0x0500
    if (getEnvir()->isDisabled()) {
#else
    if (getEnvir()->isExpressMode()) {
#endif
        // in express mode, we don't bother to update the display
        // (std::map's iteration is not very fast if map is large)
        getDisplayString().setTagArg("t", 0, "");
        return;
    }

    //char buf[40];
    //sprintf(buf,"%d conns", ndpAppConnMap.size());
    //getDisplayString().setTagArg("t",0,buf);
    if (isOperational)
        getDisplayString().removeTag("i2");
    else
        getDisplayString().setTagArg("i2", 0, "status/cross");

    int numINIT = 0, numCLOSED = 0, numLISTEN = 0, numSYN_SENT = 0,
            numSYN_RCVD = 0, numESTABLISHED = 0, numCLOSE_WAIT = 0,
            numLAST_ACK = 0, numFIN_WAIT_1 = 0, numFIN_WAIT_2 = 0, numCLOSING =
                    0, numTIME_WAIT = 0;

    for (auto & elem : ndpAppConnMap) {
        int state = (elem).second->getFsmState();

        switch (state) {
        case NDP_S_INIT:
            numINIT++;
            break;

        case NDP_S_CLOSED:
            numCLOSED++;
            break;

        case NDP_S_LISTEN:
            numLISTEN++;
            break;

        case NDP_S_SYN_SENT:
            numSYN_SENT++;
            break;

        case NDP_S_SYN_RCVD:
            numSYN_RCVD++;
            break;

        case NDP_S_ESTABLISHED:
            numESTABLISHED++;
            break;

        case NDP_S_CLOSE_WAIT:
            numCLOSE_WAIT++;
            break;

        case NDP_S_LAST_ACK:
            numLAST_ACK++;
            break;

        case NDP_S_FIN_WAIT_1:
            numFIN_WAIT_1++;
            break;

        case NDP_S_FIN_WAIT_2:
            numFIN_WAIT_2++;
            break;

        case NDP_S_CLOSING:
            numCLOSING++;
            break;

        case NDP_S_TIME_WAIT:
            numTIME_WAIT++;
            break;
        }
    }

    char buf2[200];
    buf2[0] = '\0';

    if (numINIT > 0)
        sprintf(buf2 + strlen(buf2), "init:%d ", numINIT);
    if (numCLOSED > 0)
        sprintf(buf2 + strlen(buf2), "closed:%d ", numCLOSED);
    if (numLISTEN > 0)
        sprintf(buf2 + strlen(buf2), "listen:%d ", numLISTEN);
    if (numSYN_SENT > 0)
        sprintf(buf2 + strlen(buf2), "syn_sent:%d ", numSYN_SENT);
    if (numSYN_RCVD > 0)
        sprintf(buf2 + strlen(buf2), "syn_rcvd:%d ", numSYN_RCVD);
    if (numESTABLISHED > 0)
        sprintf(buf2 + strlen(buf2), "estab:%d ", numESTABLISHED);
    if (numCLOSE_WAIT > 0)
        sprintf(buf2 + strlen(buf2), "close_wait:%d ", numCLOSE_WAIT);
    if (numLAST_ACK > 0)
        sprintf(buf2 + strlen(buf2), "last_ack:%d ", numLAST_ACK);
    if (numFIN_WAIT_1 > 0)
        sprintf(buf2 + strlen(buf2), "fin_wait_1:%d ", numFIN_WAIT_1);
    if (numFIN_WAIT_2 > 0)
        sprintf(buf2 + strlen(buf2), "fin_wait_2:%d ", numFIN_WAIT_2);
    if (numCLOSING > 0)
        sprintf(buf2 + strlen(buf2), "closing:%d ", numCLOSING);
    if (numTIME_WAIT > 0)
        sprintf(buf2 + strlen(buf2), "time_wait:%d ", numTIME_WAIT);

    getDisplayString().setTagArg("t", 0, buf2);
}

NDPConnection *Ndp::findConnForSegment(NDPSegment *ndpseg, L3Address srcAddr, L3Address destAddr) {
    SockPair key;
    key.localAddr = destAddr;
     key.remoteAddr = srcAddr;
    key.localPort = ndpseg->getDestPort();
    key.remotePort = ndpseg->getSrcPort();
    SockPair save = key;

    EV_INFO << "   eeee localPort   " <<  key.localPort  << " \n";
    EV_INFO << " eee localAddr  " <<  destAddr  << " \n";
    EV_INFO << " eee destAddr " <<  srcAddr << " \n";
    EV_INFO << " eee remotePort  " <<  key.remotePort  << " \n";

    // try with fully qualified SockPair
    auto i = ndpConnMap.find(key);
    if (i != ndpConnMap.end())
        return i->second;

    // try with localAddr missing (only localPort specified in passive/active open)
    key.localAddr = L3Address();

    i = ndpConnMap.find(key);
    if (i != ndpConnMap.end())
        return i->second;

    // try fully qualified local socket + blank remote socket (for incoming SYN)
    key = save;
    key.remoteAddr = L3Address();
    key.remotePort = -1;
    i = ndpConnMap.find(key);
    if (i != ndpConnMap.end())
        return i->second;

    // try with blank remote socket, and localAddr missing (for incoming SYN)
    key.localAddr = L3Address();
    i = ndpConnMap.find(key);
    if (i != ndpConnMap.end())
        return i->second;
    // given up
    return nullptr;
}



NDPConnection *Ndp::findConnForApp(int appGateIndex, int connId) {
    AppConnKey key;
    key.appGateIndex = appGateIndex;
    key.connId = connId;

    auto i = ndpAppConnMap.find(key);
    return i == ndpAppConnMap.end() ? nullptr : i->second;
}

ushort Ndp::getEphemeralPort() {
    // start at the last allocated port number + 1, and search for an unused one
    ushort searchUntil = lastEphemeralPort++;
    if (lastEphemeralPort == EPHEMERAL_PORTRANGE_END) // wrap
        lastEphemeralPort = EPHEMERAL_PORTRANGE_START;

    while (usedEphemeralPorts.find(lastEphemeralPort)
            != usedEphemeralPorts.end()) {
        if (lastEphemeralPort == searchUntil) // got back to starting point?
            throw cRuntimeError(
                    "Ephemeral port range %d..%d exhausted, all ports occupied",
                    EPHEMERAL_PORTRANGE_START, EPHEMERAL_PORTRANGE_END);

        lastEphemeralPort++;

        if (lastEphemeralPort == EPHEMERAL_PORTRANGE_END) // wrap
            lastEphemeralPort = EPHEMERAL_PORTRANGE_START;
    }

    // found a free one, return it
    return lastEphemeralPort;
}

void Ndp::addSockPair(NDPConnection *conn, L3Address localAddr,
        L3Address remoteAddr, int localPort, int remotePort) {
    // update addresses/ports in NDPConnection
    SockPair key;
    key.localAddr = conn->localAddr = localAddr;
    key.remoteAddr = conn->remoteAddr = remoteAddr;
    key.localPort = conn->localPort = localPort;
    key.remotePort = conn->remotePort = remotePort;

   EV_INFO << "aaaa  localAddr " <<  localAddr << " \n";
   EV_INFO << "aaa remoteAddr   " <<  remoteAddr  << " \n";
   EV_INFO << "aaa localPort  " <<  localPort  << " \n";
   EV_INFO << "aaa remotePort  " <<  remotePort  << " \n";


    // make sure connection is unique
    auto it = ndpConnMap.find(key);
    if (it != ndpConnMap.end()) {
        // throw "address already in use" error
        if (remoteAddr.isUnspecified() && remotePort == -1)
            throw cRuntimeError(
                    "Address already in use: there is already a connection listening on %s:%d",
                    localAddr.str().c_str(), localPort);
        else
            throw cRuntimeError(
                    "Address already in use: there is already a connection %s:%d to %s:%d",
                    localAddr.str().c_str(), localPort,
                    remoteAddr.str().c_str(), remotePort);
    }

    // then insert it into ndpConnMap
    ndpConnMap[key] = conn;

    // mark port as used
    if (localPort >= EPHEMERAL_PORTRANGE_START  && localPort < EPHEMERAL_PORTRANGE_END)
        usedEphemeralPorts.insert(localPort);
}



void Ndp::updateSockPair(NDPConnection *conn, L3Address localAddr,
        L3Address remoteAddr, int localPort, int remotePort) {
    // find with existing address/port pair...
    SockPair key;
    key.localAddr = conn->localAddr;
    key.remoteAddr = conn->remoteAddr;
    key.localPort = conn->localPort;
    key.remotePort = conn->remotePort;
    auto it = ndpConnMap.find(key);

    ASSERT(it != ndpConnMap.end() && it->second == conn);

    // ...and remove from the old place in ndpConnMap
    ndpConnMap.erase(it);

    // then update addresses/ports, and re-insert it with new key into ndpConnMap
    key.localAddr = conn->localAddr = localAddr;
    key.remoteAddr = conn->remoteAddr = remoteAddr;
    ASSERT(conn->localPort == localPort);
    key.remotePort = conn->remotePort = remotePort;
    ndpConnMap[key] = conn;


    EV_INFO << "bbbb updateSockPair localAddr " <<  localAddr << " \n";
    EV_INFO << "bbbb updateSockPair remoteAddr   " <<  remoteAddr  << " \n";
    EV_INFO << "bbbb updateSockPair localPort  " <<  localPort  << " \n";
    EV_INFO << "bbbb updateSockPair remotePort  " <<  remotePort  << " \n";
    // localPort doesn't change (see ASSERT above), so there's no need to update usedEphemeralPorts[].
}



void Ndp::removeConnection(NDPConnection *conn) {
    EV_INFO << "Deleting NDP connectionnn \n";

    AppConnKey key;
    key.appGateIndex = conn->appGateIndex;
    key.connId = conn->connId;
    ndpAppConnMap.erase(key);

    SockPair key2;
    key2.localAddr = conn->localAddr;
    key2.remoteAddr = conn->remoteAddr;
    key2.localPort = conn->localPort;
    key2.remotePort = conn->remotePort;
    ndpConnMap.erase(key2);

    // IMPORTANT: usedEphemeralPorts.erase(conn->localPort) is NOT GOOD because it
    // deletes ALL occurrences of the port from the multiset.
    auto it = usedEphemeralPorts.find(conn->localPort);

    if (it != usedEphemeralPorts.end())
        usedEphemeralPorts.erase(it);

    delete conn;
}

void Ndp::finish() {
    if (requestTimerMsg->isScheduled())
        cancelEvent(requestTimerMsg);
      delete requestTimerMsg;

    EV_INFO << getFullPath() << ": finishing with " << ndpConnMap.size() << " connections open.\n";
}

NDPSendQueue *Ndp::createSendQueue() {
    return new NDPMsgBasedSendQueue(); // oooooooooooo
}

NDPReceiveQueue *Ndp::createReceiveQueue() {
//    return new NDPSymbolsRcvQueue();
}

bool Ndp::handleOperationStage(LifecycleOperation *operation, int stage,
        IDoneCallback *doneCallback) {
    Enter_Method_Silent();

    if (dynamic_cast<NodeStartOperation *>(operation)) {
        if ((NodeStartOperation::Stage)stage == NodeStartOperation::STAGE_TRANSPORT_LAYER) {
            //FIXME implementation
            isOperational = true;
            updateDisplayString();
        }
    }
    else if (dynamic_cast<NodeShutdownOperation *>(operation)) {
        if ((NodeShutdownOperation::Stage)stage == NodeShutdownOperation::STAGE_TRANSPORT_LAYER) {
            //FIXME close connections???
            reset();
            isOperational = false;
            updateDisplayString();
        }
    }
    else if (dynamic_cast<NodeCrashOperation *>(operation)) {
        if ((NodeCrashOperation::Stage)stage == NodeCrashOperation::STAGE_CRASH) {
            //FIXME implementation
            reset();
            isOperational = false;
            updateDisplayString();
        }
    }
    else {
        throw cRuntimeError("Unsupported operation '%s'", operation->getClassName());
    }

    return true;
}

void Ndp::reset() {
    for (auto & elem : ndpAppConnMap)
        delete elem.second;
    ndpAppConnMap.clear();
    ndpConnMap.clear();
    usedEphemeralPorts.clear();
    lastEphemeralPort = EPHEMERAL_PORTRANGE_START;
}








void Ndp::sendFirstRequest() {
//       std::cout << "  sendFirstRequest  \n";
    bool allEmpty = allPullQueuesEmpty();
//    if (allEmpty == false && test == true) {   // ?????? why test is here
    if (allEmpty == false ) {
         requestTimer();

    //    process_REQUEST_TIMER();
//        test = false;
    }
}


// MOH added
void Ndp::printConnRequestMap() {
//    std::cout << "   printConnRequestMap   "     << "\n";
    auto iterrr = requestCONNMap.begin();
    int index = 0;
    while (iterrr != requestCONNMap.end()) {
//          EV << index << " printConnRequestMap \n " ;
//          EV <<  " connIndex = " <<  iterrr->first << " \n " ;
//          EV << "  connId=  " << iterrr->second->connId  << "\n" ;
//          EV << " getPullsQueueLength()=  " << iterrr->second->getPullsQueueLength() << "\n" ;
//          EV << " rcvdSymbols =  " << iterrr->second->getNumRcvdSymbols() <<  "\n\n\n" ;
//          EV << " requestCONNMap.size() =  " << requestCONNMap.size() <<  "\n\n\n" ;

//          std::cout << " requestCONNMap.size() =  " << requestCONNMap.size() <<  "\n\n\n" ;

//        if (iterrr->second->getNumRcvdSymbols() > 0 && test == true) {
//            requestTimer();
//            test = false;
//        }
        index++;
        iterrr++;
    }

}


// returns true if all Pull queues are empty; otherwise returns false
bool Ndp::allPullQueuesEmpty() {
//     std::cout << "  Ndp::allPullQueuesEmpty()    "  << this->getFullPath()   << "\n";
    int pullsQueueLength =0;
    auto iter = requestCONNMap.begin();
    while (iter !=  requestCONNMap.end()){
//        std::cout << "   aaaaallPullQueuesEmpty   "     << "\n";
         pullsQueueLength = iter->second->getPullsQueueLength();
//         std::cout <<    this->getFullPath()   << " pullsQueueLength=   "  <<   pullsQueueLength<< "\n";

        if (pullsQueueLength >0) return false;
        ++iter;
    }
      return true;
}

// returns true if all connections are finished; otherwise returns false
bool Ndp::allConnFinished() {
//     std::cout << "  allConnFinished ?   "     << "\n";
    bool connDone;

    auto iter = requestCONNMap.begin();
    int ii=0;
    while (iter != requestCONNMap.end()) {
        connDone = iter->second->isConnFinished();
//        std::cout << "  allConnFinished ssss  "    << requestCONNMap.size() << "\n";
      if (connDone == false) {
//            std::cout << "allConnFinished FALSE FFFFFFFFFFF, connIndex=  " << ii << " ,numConn = " << requestCONNMap.size() << "\n";
            return false;
        }
        ++iter;
        ++ii;
    }
    cancelRequestTimer();
    return true;
}



void Ndp::updateConnMap() {
    std::cout << "  updateConnMap updateConnMap   "     << "\n";
    a:
    bool connDone;
    auto iter = requestCONNMap.begin();

    while (iter != requestCONNMap.end()) {
        connDone = iter->second->isConnFinished();
        if (connDone == true)  {
            requestCONNMap.erase(iter);
            goto a;
        }
         ++iter;
   }
}


void Ndp::requestTimer() {
//    std::cout << "  requestTimer  \n";
//  //  state->request_rexmit_count = 0;
//  //  state->request_rexmit_timeout = NDP_TIMEOUT_PULL_REXMIT;  // 3 sec
//    std::cout << " start requestTimer... \n ";
//    EV << " start requestTimer... \n ";
    cancelRequestTimer();
    simtime_t requestTime = (simTime() + SimTime( PACING_TIME , SIMTIME_US)); // pacing
//    if (allConnFinished() == false)
     scheduleAt(requestTime, requestTimerMsg); // 0.000009
//     requestTimerStamps.record(requestTime);

//    if (allConnFinished() == true)
//        cancelRequestTimer();
}

void Ndp::cancelRequestTimer() {
//    std::cout << " cancelRequestTimer  "   << "\n";
    if (requestTimerMsg->isScheduled())
        cancelEvent(requestTimerMsg);
//    delete requestTimerMsg;
}

bool Ndp::getNapState() {
    return nap;
}


void Ndp::process_REQUEST_TIMER() {
//   std::cout << "  \n process_REQUEST_TIMER " << this->getFullPath() << " \n";
    bool sendNewRequest = false;
    auto iter = requestCONNMap.begin();
    bool allEmpty = allPullQueuesEmpty();
    bool allDone = allConnFinished();

    if (allDone == true) {
        cancelRequestTimer();
    } else if (allDone == false && allEmpty == true) {
        ++times;
//         requestTimer(); // ?????????????????????
//        if (times == 50000) {
//            times=0;
//            cancelRequestTimer();
//        }
        nap = true;
    } else if (allDone == false && allEmpty == false) {
        times = 0;
        nap = false;
        while (sendNewRequest != true) {
            if (counter == requestCONNMap.size())
                counter = 0;
            auto iter = requestCONNMap.begin();
            std::advance(iter, counter);
            int pullsQueueLength = iter->second->getPullsQueueLength();
            if (pullsQueueLength > 0) {
                iter->second->sendRequestFromPullsQueue();
                requestTimer();
                sendNewRequest = true;
            }
            ++counter;
        }
    }
}


// destructor
Ndp::~Ndp() {
    while (!ndpAppConnMap.empty()) {
        auto i = ndpAppConnMap.begin();
        delete i->second;
        ndpAppConnMap.erase(i);
    }

}


} // namespace ndp
} // namespace inet
