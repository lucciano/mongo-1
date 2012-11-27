// s_only.cpp

/*    Copyright 2009 10gen Inc.
 *    Copyright (C) 2013 Tokutek Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"

#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/auth_external_state_impl.h"
#include "mongo/s/shard.h"
#include "mongo/s/grid.h"
#include "request.h"
#include "client_info.h"
#include "../db/dbhelpers.h"
#include "../db/matcher.h"
#include "../db/commands.h"

/*
  most a pile of hacks to make linking nicer

 */
namespace mongo {

    void* remapPrivateView(void *oldPrivateAddr) {
        log() << "remapPrivateView called in mongos, aborting" << endl;
        fassertFailed(16462);
    }

    /** When this callback is run, we record a shard that we've used for useful work
     *  in an operation to be read later by getLastError()
    */
    void usingAShardConnection( const string& addr ) {
        ClientInfo::get()->addShard( addr );
    }

    TSP_DEFINE(Client,currentClient)

    LockState::LockState(){} // ugh

    Client::Client(const char *desc , AbstractMessagingPort *p) :
        ClientBasic(p),
        _context(0),
        _shutdown(false),
        _desc(desc),
        _god(0),
        _lastGTID() {
    }
    Client::~Client() {}
    bool Client::shutdown() { return true; }

    void ClientBasic::initializeAuthorizationManager() {
        std::string adminNs = "admin";
        DBConfigPtr config = grid.getDBConfig(adminNs);
        Shard shard = config->getShard(adminNs);
        scoped_ptr<ScopedDbConnection> connPtr(
                ScopedDbConnection::getInternalScopedDbConnection(shard.getConnString(), 30.0));
        ScopedDbConnection& conn = *connPtr;

        //
        // Note: The connection mechanism here is *not* ideal, and should not be used elsewhere.
        // It is safe in this particular case because the admin database is always on the config
        // server and does not move.
        //

        AuthorizationManager* authManager = new AuthorizationManager(new AuthExternalStateImpl());
        Status status = authManager->initialize(conn.get());
        massert(16820,
                mongoutils::str::stream() << "Error initializing AuthorizationManager: "
                                          << status.reason(),
                status == Status::OK());
        setAuthorizationManager(authManager);
    }

    Client& Client::initThread(const char *desc, AbstractMessagingPort *mp) {
        // mp is non-null only for client connections, and mongos uses ClientInfo for those
        massert(16817, "Client being used for incoming connection thread in mongos", mp == NULL);
        setThreadName(desc);
        verify( currentClient.get() == 0 );
        // mp is always NULL in mongos. Threads for client connections use ClientInfo in mongos
        massert(16816,
                "Non-null messaging port provided to Client::initThread in a mongos",
                mp == NULL);
        Client *c = new Client(desc, mp);
        currentClient.reset(c);
        mongo::lastError.initThread();
        return *c;
    }

    string Client::clientAddress(bool includePort) const {
        ClientInfo * ci = ClientInfo::get();
        if ( ci )
            return ci->getRemote();
        return "";
    }

    bool execCommand( Command * c ,
                      Client& client , int queryOptions ,
                      const char *ns, BSONObj& cmdObj ,
                      BSONObjBuilder& result,
                      bool fromRepl ) {
        verify(c);

        string dbname = nsToDatabase( ns );

        if ( cmdObj["help"].trueValue() ) {
            stringstream ss;
            ss << "help for: " << c->name << " ";
            c->help( ss );
            result.append( "help" , ss.str() );
            result.append( "lockType" , c->locktype() );
            return true;
        }

        if ( c->adminOnly() ) {
            if ( dbname != "admin" ) {
                result.append( "errmsg" ,  "access denied- use admin db" );
                log() << "command denied: " << cmdObj.toString() << endl;
                return false;
            }
            LOG( 2 ) << "command: " << cmdObj << endl;
        }

        if (!client.getAuthenticationInfo()->isAuthorized(dbname)) {
            result.append("errmsg" , "unauthorized");
            result.append("note" , "from execCommand" );
            return false;
        }

        string errmsg;
        int ok = c->run( dbname , cmdObj , queryOptions, errmsg , result , fromRepl );
        if ( ! ok )
            result.append( "errmsg" , errmsg );
        return ok;
    }
}
