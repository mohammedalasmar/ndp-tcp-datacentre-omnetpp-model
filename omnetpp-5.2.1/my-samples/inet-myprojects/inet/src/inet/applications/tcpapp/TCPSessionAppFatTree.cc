//
// Copyright (C) 2004 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include "inet/applications/tcpapp/TCPSessionAppFatTree.h"

#include "inet/common/RawPacket.h"
#include "inet/networklayer/common/L3AddressResolver.h"
#include "inet/common/ModuleAccess.h"
#include "inet/common/lifecycle/NodeOperations.h"

namespace inet {

Define_Module(TCPSessionAppFatTree);

#define MSGKIND_CONNECT    1
#define MSGKIND_SEND       2
#define MSGKIND_CLOSE      3

TCPSessionAppFatTree::~TCPSessionAppFatTree()
{
    cancelAndDelete(timeoutMsg);
    cancelAndDelete(mohMsg);

}

void TCPSessionAppFatTree::initialize(int stage)
{
//    std::cout << " ini " << stage << std::endl;

    TCPAppBase::initialize(stage);
    if (stage == INITSTAGE_LOCAL) {
        activeOpen = par("active");
//        tOpen = par("tOpen");   // commented moh
//        tSend = par("tSend");  // commented moh
//        sendBytes = par("sendBytes");   // commented moh

        tOpen = &par("tOpen");   // volatile  moh new
        tSend = &par("tSend");   // volatile  moh new
        sendBytes = &par("sendBytes");  // volatile  moh new

        tClose = par("tClose");

        std::cout << "initialize -- tOpen =   " << tSend->doubleValue() << "\n";
        std::cout << "initialize -- tSend =   " << tSend->doubleValue() << "\n";
        std::cout << "initialize -- sendBytes  " << sendBytes->doubleValue() << "\n";
        const char *connectAddress = par("connectAddress");
        std::cout << "initialize -- connectAddress  " << connectAddress << "\n";
        int connectPort = par("connectPort");
        std::cout << "initialize -- connectPort  " << connectPort << "\n";

        commandIndex = 0;

        socket.readDataTransferModePar(*this);

        const char *script = par("sendScript");
        parseScript(script);

        if (sendBytes->doubleValue() > 0 && commands.size() > 0)
            throw cRuntimeError("Cannot use both sendScript and tSend+sendBytes");

        // moh : commented see TCPSessionAppFatTree::handleParameterChange
//        if (sendBytes > 0)
//            commands.push_back(Command(  (simtime_t) tSend->doubleValue(), sendBytes));
//        if (commands.size() == 0)
//            throw cRuntimeError("sendScript is empty");

    }
    else if (stage == INITSTAGE_APPLICATION_LAYER) {
        timeoutMsg = new cMessage("timer");
        mohMsg = new cMessage("mohMsg");

        nodeStatus = dynamic_cast<NodeStatus *>(findContainingNode(this)->getSubmodule("status"));

        if (isNodeUp()) {
            timeoutMsg->setKind(MSGKIND_CONNECT);
//            mohMsg->setKind(MSGKIND_CONNECT);  // add moh
//            scheduleAt( (simtime_t) tOpen->doubleValue(), timeoutMsg);  // moh comment this line as we need to start the connection based on handleParameterChange()
        }
    }
}

bool TCPSessionAppFatTree::isNodeUp()
{
    return !nodeStatus || nodeStatus->getState() == NodeStatus::UP;
}

bool TCPSessionAppFatTree::handleOperationStage(LifecycleOperation *operation, int stage, IDoneCallback *doneCallback)
{
    std::cout << " ttttttttttttttttttttttttttttttttttttttttttttt \n";
    Enter_Method_Silent();
    if (dynamic_cast<NodeStartOperation *>(operation)) {
        if ((NodeStartOperation::Stage) stage == NodeStartOperation::STAGE_APPLICATION_LAYER) {
            if (simTime() <= (simtime_t) tOpen->doubleValue()) {
                timeoutMsg->setKind(MSGKIND_CONNECT);
                scheduleAt((simtime_t) tOpen->doubleValue(), timeoutMsg);
            }
        }
    }
    else if (dynamic_cast<NodeShutdownOperation *>(operation)) {
        if ((NodeShutdownOperation::Stage) stage == NodeShutdownOperation::STAGE_APPLICATION_LAYER) {
            cancelEvent(timeoutMsg);
            if (socket.getState() == TCPSocket::CONNECTED || socket.getState() == TCPSocket::CONNECTING || socket.getState() == TCPSocket::PEER_CLOSED)
                close();
            // TODO: wait until socket is closed
        }
    }
    else if (dynamic_cast<NodeCrashOperation *>(operation)) {
        if ((NodeCrashOperation::Stage) stage == NodeCrashOperation::STAGE_CRASH)
            cancelEvent(timeoutMsg);
    }
    else
        throw cRuntimeError("Unsupported lifecycle operation '%s'", operation->getClassName());
    return true;
}

void TCPSessionAppFatTree::handleTimer(cMessage *msg)
{
//    std::cout<<"TCPSessionAppFatTree::handleTimer " << msg->getKind() << "\n" ;

    switch (msg->getKind()) {
        case MSGKIND_CONNECT:

            if (activeOpen)
                connect(); // sending will be scheduled from socketEstablished()
            else
                ; //TODO
            break;

        case MSGKIND_SEND:
            sendData();
            break;

        case MSGKIND_CLOSE:

            close();
            break;

        default:
            throw cRuntimeError("Invalid timer msg: kind=%d", msg->getKind());
    }
}

void TCPSessionAppFatTree::sendData()
{
//    std::cout <<"sendData() \n";

    long numBytes = commands[commandIndex].numBytes;
    EV_INFO << "sending data with " << numBytes << " bytes\n";
    sendPacket(createDataPacket(numBytes));

    if (++commandIndex < (int) commands.size()) {
        simtime_t tSend = commands[commandIndex].tSend;
        scheduleAt(std::max(tSend, simTime()), timeoutMsg);
    }
    else {
        timeoutMsg->setKind(MSGKIND_CLOSE);
        scheduleAt(std::max(tClose, simTime()), timeoutMsg);
    }
}

cPacket *TCPSessionAppFatTree::createDataPacket(long sendBytes)
{
    switch (socket.getDataTransferMode()) {
        case TCP_TRANSFER_BYTECOUNT:
        case TCP_TRANSFER_OBJECT: {
            cPacket *msg = nullptr;
            msg = new cPacket("data1");
            msg->setByteLength(sendBytes);
            return msg;
        }

        case TCP_TRANSFER_BYTESTREAM: {
            RawPacket *msg = new RawPacket("data1");
            unsigned char *ptr = new unsigned char[sendBytes];

            for (int i = 0; i < sendBytes; i++)
                ptr[i] = (bytesSent + i) & 0xFF;

            msg->getByteArray().assignBuffer(ptr, sendBytes);
            msg->setByteLength(sendBytes);
            return msg;
        }

        default:
            throw cRuntimeError("Invalid TCP data transfer mode: %d", socket.getDataTransferMode());
    }
}

void TCPSessionAppFatTree::socketEstablished(int connId, void *ptr)
{
    TCPAppBase::socketEstablished(connId, ptr);

    ASSERT(commandIndex == 0);
    timeoutMsg->setKind(MSGKIND_SEND);
    simtime_t tSend = commands[commandIndex].tSend;
    scheduleAt(std::max(tSend, simTime()), timeoutMsg);
}

void TCPSessionAppFatTree::socketDataArrived(int connId, void *ptr, cPacket *msg, bool urgent)
{
    TCPAppBase::socketDataArrived(connId, ptr, msg, urgent);
}

void TCPSessionAppFatTree::socketClosed(int connId, void *ptr)
{
    TCPAppBase::socketClosed(connId, ptr);
    cancelEvent(timeoutMsg);
}

void TCPSessionAppFatTree::socketFailure(int connId, void *ptr, int code)
{
    TCPAppBase::socketFailure(connId, ptr, code);
    cancelEvent(timeoutMsg);
}

void TCPSessionAppFatTree::parseScript(const char *script)
{
    const char *s = script;

    EV_DEBUG << "parse script \"" << script << "\"\n";
    while (*s) {
        // parse time
        while (isspace(*s))
            s++;

        if (!*s || *s == ';')
            break;

        const char *s0 = s;
        simtime_t tSend = strtod(s, &const_cast<char *&>(s));

        if (s == s0)
            throw cRuntimeError("Syntax error in script: simulation time expected");

        // parse number of bytes
        while (isspace(*s))
            s++;

        if (!isdigit(*s))
            throw cRuntimeError("Syntax error in script: number of bytes expected");

        long numBytes = strtol(s, nullptr, 10);

        while (isdigit(*s))
            s++;

        // add command
        EV_DEBUG << " add command (" << tSend << "s, " << "B)\n";
        commands.push_back(Command(tSend, numBytes));

        // skip delimiter
        while (isspace(*s))
            s++;

        if (!*s)
            break;

        if (*s != ';')
            throw cRuntimeError("Syntax error in script: separator ';' missing");

        s++;

        while (isspace(*s))
            s++;
    }
    EV_DEBUG << "parser finished\n";
}

void TCPSessionAppFatTree::finish()
{
    EV << getFullPath() << ": received " << bytesRcvd << " bytes in " << packetsRcvd << " packets\n";
    recordScalar("bytesRcvd", bytesRcvd);
    recordScalar("bytesSent", bytesSent);
//    std::cout << "finish TCPSessionAppFatTree  " << "\n";

}

void TCPSessionAppFatTree::refreshDisplay() const
{
    std::ostringstream os;
    os << TCPSocket::stateName(socket.getState()) << "\nsent: " << bytesSent << " bytes\nrcvd: " << bytesRcvd << " bytes";
    getDisplayString().setTagArg("t", 0, os.str().c_str());
}

void TCPSessionAppFatTree::handleParameterChange(const char *parname)
{

    if (parname && strcmp(parname, "tSend") == 0) {
        std::cout << " Good TO See YOU YAAAAAAAA " << "  \n\n";

//        cancelEvent(timeoutMsg);
        tSend = &par("tSend"); // refresh data member
        tOpen = tSend;

        //      timeoutMsg->setKind(MSGKIND_CONNECT);
//       cSimpleModule::initialize();
//       connect();
        if (sendBytes->doubleValue() > 0)
            commands.push_back(Command((simtime_t) tSend->doubleValue(), sendBytes->doubleValue()));
        scheduleAt((simtime_t) tOpen->doubleValue(), timeoutMsg);

        std::cout << "new -- tOpen =   " << tOpen->doubleValue() << "\n";
        std::cout << "new -- tSend =   " << tSend->doubleValue() << "\n";
        std::cout << "new -- sendBytes  " << sendBytes->doubleValue() << "\n";
        const char *connectAddress = par("connectAddress");
        std::cout << "new -- connectAddress  " << connectAddress << "\n";
        int connectPort = par("connectPort");
        std::cout << "new -- connectPort " << connectPort << "\n";

    }

//    std::cout << "tOpen  " << tOpen->doubleValue() << "\n";

}

} // namespace inet

