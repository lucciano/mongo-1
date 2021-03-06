// client.cpp

/**
*    Copyright (C) 2009 10gen Inc.
*    Copyright (C) 2013 Tokutek Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* Client represents a connection to the database (the server-side) and corresponds
   to an open socket (or logical connection if pooling on sockets) from a client.
*/

#include "mongo/pch.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/databaseholder.h"
#include "mongo/db/json.h"
#include "mongo/db/security.h"
#include "mongo/db/commands.h"
#include "mongo/db/instance.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/db/repl/rs.h"
#include "mongo/s/d_logic.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/mongoutils/html.h"
#include "mongo/util/mongoutils/checksum.h"

namespace mongo {
  
    mongo::mutex& Client::clientsMutex = *(new mutex("clientsMutex"));
    set<Client*>& Client::clients = *(new set<Client*>); // always be in clientsMutex when manipulating this

    RWLockRecursive operationLock("operationLock");


    TSP_DEFINE(Client, currentClient)

    /* each thread which does db operations has a Client object in TLS.
       call this when your thread starts.
    */
#if defined _DEBUG
    static unsigned long long nThreads = 0;
    void assertStartingUp() { 
        verify( nThreads <= 1 );
    }
#else
    void assertStartingUp() { }
#endif

    Client& Client::initThread(const char *desc, AbstractMessagingPort *mp) {
#if defined(_DEBUG)
        nThreads++; // never decremented.  this is for casi class asserts
#endif
        verify( currentClient.get() == 0 );
        Client *c = new Client(desc, mp);
        currentClient.reset(c);
        mongo::lastError.initThread();
        return *c;
    }

    Client::Client(const char *desc, AbstractMessagingPort *p) :
        _context(0),
        _shutdown(false),
        _desc(desc),
        _god(0),
        _mp(p)
    {
        _connectionId = p ? p->connectionId() : 0;
        
        if ( str::equals( "conn" , desc ) && _connectionId > 0 )
            _desc = str::stream() << desc << _connectionId;
        setThreadName(_desc.c_str());
        _curOp = new CurOp( this );
#ifndef _WIN32
        stringstream temp;
        temp << hex << showbase << pthread_self();
        _threadId = temp.str();
#endif
        scoped_lock bl(clientsMutex);
        clients.insert(this);
    }

    Client::~Client() {
        _god = 0;

        // client is being destroyed, if there are any transactions on our stack,
        // abort them, starting with the one in the loadInfo object if it exists.
        _loadInfo.reset();
        if (_transactions) {
            while (_transactions->hasLiveTxn()) {
                _transactions->abortTxn();
            }
        }

        if ( _context )
            error() << "Client::~Client _context should be null but is not; client:" << _desc << endl;

        if ( ! _shutdown ) {
            error() << "Client::shutdown not called: " << _desc << endl;
        }

        if ( ! inShutdown() ) {
            // we can't clean up safely once we're in shutdown
            scoped_lock bl(clientsMutex);
            if ( ! _shutdown )
                clients.erase(this);
            delete _curOp;
        }
    }

    // called when we transition from primary to secondary
    // a global write lock is held while this is happening
    void Client::abortLiveTransactions() {
        verify(Lock::isW());
        scoped_lock bl(Client::clientsMutex);
        for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) {
            Client *c = *i;
            while (c->hasTxn()) {
                c->abortTopTxn();
            }
        }
    }

    bool Client::shutdown() {
        _shutdown = true;
        // client is being destroyed, if there are any transactions on our stack,
        // abort them, starting with the one in the loadInfo object if it exists.
        _loadInfo.reset();
        if (_transactions) {
            while (_transactions->hasLiveTxn()) {
                _transactions->abortTxn();
            }
        }

        if ( inShutdown() ) {
            return false;
        }
        {
            scoped_lock bl(clientsMutex);
            clients.erase(this);
        }

        return false;
    }

    BSONObj CachedBSONObj::_tooBig = fromjson("{\"$msg\":\"query not recording (too large)\"}");
    Client::Context::Context( const StringData &ns , Database * db, bool doauth ) :
        _client( currentClient.get() ), 
        _oldContext( _client->_context ),
        _path( mongo::dbpath ), // is this right? could be a different db? may need a dassert for this
        _doVersion( true ),
        _ns( ns.toString() ),
        _db(db)
    {
        _client->_context = this;
        checkNsAccess( doauth );
    }

    Client::Context::Context(const StringData &ns, const StringData &path , bool doauth, bool doVersion ) :
        _client( currentClient.get() ),
        _oldContext( _client->_context ),
        _path( path.toString() ),
        _doVersion(doVersion),
        _ns( ns.toString() ),
        _db(0)
    {
        _finishInit( doauth );
    }

    /** "read lock, and set my context, all in one operation" 
     *  This handles (if not recursively locked) opening an unopened database.
     */
    Client::ReadContext::ReadContext(const StringData &ns, const StringData &path, bool doauth)
        : _lk( ns ) ,
          _c( ns , path , doauth ) {
    }

    Client::WriteContext::WriteContext(const StringData &ns, const StringData &path , bool doauth ) 
        : _lk( ns ) ,
          _c( ns , path , doauth ) {
    }


    void Client::Context::checkNotStale() const { 
        switch ( _client->_curOp->getOp() ) {
        case dbGetMore: // getMore's are special and should be handled else where
        case dbUpdate: // update & delete check shard version in instance.cpp, so don't check here as well
        case dbDelete:
            break;
        default: {
            string errmsg;
            ShardChunkVersion received;
            ShardChunkVersion wanted;
            if ( ! shardVersionOk( _ns , errmsg, received, wanted ) ) {
                ostringstream os;
                os << "[" << _ns << "] shard version not ok in Client::Context: " << errmsg;
                throw SendStaleConfigException( _ns, os.str(), received, wanted );
            }
        }
        }
    }

    // invoked from ReadContext
    Client::Context::Context(const StringData &path, const StringData &ns, Database *db , bool doauth) :
        _client( currentClient.get() ),
        _oldContext( _client->_context ),
        _path( path.toString() ),
        _doVersion( true ),
        _ns( ns.toString() ),
        _db(db)
    {
        verify(_db);
        checkNotStale();
        _client->_context = this;
        _client->_curOp->enter( this );
        checkNsAccess( doauth );
    }

    void Client::Context::_finishInit( bool doauth ) {
        dassert( Lock::isLocked() );
        int writeLocked = Lock::somethingWriteLocked();

        _db = dbHolderUnchecked().getOrCreate( _ns , _path );
        verify(_db);
        if( _doVersion ) checkNotStale();
        massert( 16107 , str::stream() << "Don't have a lock on: " << _ns , Lock::atLeastReadLocked( _ns ) );
        _client->_context = this;
        _client->_curOp->enter( this );
        checkNsAccess( doauth, writeLocked ? 1 : 0 );
    }

    void Client::Context::_auth( int lockState ) {
        if (lockState <= 0 && str::endsWith(_ns, ".system.users"))
            lockState = 1; // we don't want read-only users to be able to read system.users SERVER-4692

        if ( _client->_ai.isAuthorizedForLock( _db->name() , lockState ) )
            return;

        // before we assert, do a little cleanup
        _client->_context = _oldContext; // note: _oldContext may be null

        stringstream ss;
        ss << "unauthorized db:" << _db->name() << " ns:" << _ns << " lock type:" << lockState << " client:" << _client->clientAddress();
        uasserted( 10057 , ss.str() );
    }
    
    Client::Context::~Context() {
        DEV verify( _client == currentClient.get() );
        _client->_curOp->recordGlobalTime( _timer.micros() );
        _client->_curOp->leave( this );
        _client->_context = _oldContext; // note: _oldContext may be null
    }

    bool Client::Context::inDB( const StringData& db , const StringData& path ) const {
        if ( _path != path )
            return false;

        if ( db == _ns )
            return true;

        size_t idx = _ns.find( db.toString() );
        if ( idx != 0 )
            return false;

        return  _ns[db.size()] == '.';
    }
    
    void Client::Context::checkNsAccess( bool doauth, int lockState ) {
        if ( 0 ) { // SERVER-4276
            uassert( 15929, "client access to index backing namespace prohibited", NamespaceString::normal( _ns.c_str() ) );
        }
        if ( doauth ) {
            _auth( lockState );
        }
    }
    void Client::Context::checkNsAccess( bool doauth ) {
        checkNsAccess( doauth, Lock::somethingWriteLocked() ? 1 : 0 );
    }

    void Client::appendLastGTID( BSONObjBuilder& b ) const {
        // _lastGTID is never set if replication is off
        if( theReplSet || ! _lastGTID.isInitial()) {
            addGTIDToBSON("lastGTID", _lastGTID, b);
        }
    }

    string Client::clientAddress(bool includePort) const {
        if( _curOp )
            return _curOp->getRemoteString(includePort);
        return "";
    }

    string Client::toString() const {
        stringstream ss;
        if ( _curOp )
            ss << _curOp->infoNoauth().jsonString();
        return ss.str();
    }

    string sayClientState() {
        Client* c = currentClient.get();
        if ( !c )
            return "no client";
        return c->toString();
    }

    void KillCurrentOp::interruptJs( AtomicUInt *op ) {
        if ( !globalScriptEngine )
            return;
        if ( !op ) {
            globalScriptEngine->interruptAll();
        }
        else {
            globalScriptEngine->interrupt( *op );
        }
    }

    void KillCurrentOp::killAll() {
        _globalKill = true;
        interruptJs( 0 );
    }

    void KillCurrentOp::kill(AtomicUInt i) {
        bool found = false;
        {
            scoped_lock l( Client::clientsMutex );
            for( set< Client* >::const_iterator j = Client::clients.begin(); !found && j != Client::clients.end(); ++j ) {
                for( CurOp *k = ( *j )->curop(); !found && k; k = k->parent() ) {
                    if ( k->opNum() == i ) {
                        k->kill();
                        for( CurOp *l = ( *j )->curop(); l != k; l = l->parent() ) {
                            l->kill();
                        }
                        found = true;
                    }
                }
            }
        }
        if ( found ) {
            interruptJs( &i );
        }
    }

    // used to establish a slave for 'w' write concern
    void Client::gotHandshake( const BSONObj& o ) {
        BSONObjIterator i(o);

        {
            BSONElement id = i.next();
            verify( id.type() );
            _remoteId = id.wrap( "_id" );
        }

        BSONObjBuilder b;
        while ( i.more() )
            b.append( i.next() );
        
        b.appendElementsUnique( _handshake );

        _handshake = b.obj();

        if (theReplSet && o.hasField("member")) {
            theReplSet->registerSlave(_remoteId, o["member"].Int());
        }
    }

    bool ClientBasic::hasCurrent() {
        return currentClient.get();
    }

    ClientBasic* ClientBasic::getCurrent() {
        return currentClient.get();
    }

    class HandshakeCmd : public InformationCommand {
      public:
        void help(stringstream& h) const { h << "internal"; }
        HandshakeCmd() : InformationCommand("handshake") {}
        virtual bool adminOnly() const { return false; }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            Client& c = cc();
            c.gotHandshake( cmdObj );
            return true;
        }
    } handshakeCmd;

    class ClientListPlugin : public WebStatusPlugin {
    public:
        ClientListPlugin() : WebStatusPlugin( "clients" , 20 ) {}
        virtual void init() {}

        virtual void run( stringstream& ss ) {
            using namespace mongoutils::html;

            ss << "\n<table border=1 cellpadding=2 cellspacing=0>";
            ss << "<tr align='left'>"
               << th( a("", "Connections to the database, both internal and external.", "Client") )
               << th( a("http://dochub.mongodb.org/core/viewingandterminatingcurrentoperation", "", "OpId") )
               << "<th>Locking</th>"
               << "<th>Waiting</th>"
               << "<th>SecsRunning</th>"
               << "<th>Op</th>"
               << th( a("http://dochub.mongodb.org/core/whatisanamespace", "", "Namespace") )
               << "<th>Query</th>"
               << "<th>client</th>"
               << "<th>msg</th>"
               << "<th>progress</th>"

               << "</tr>\n";
            {
                scoped_lock bl(Client::clientsMutex);
                for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) {
                    Client *c = *i;
                    CurOp& co = *(c->curop());
                    ss << "<tr><td>" << c->desc() << "</td>";

                    tablecell( ss , co.opNum() );
                    tablecell( ss , co.active() );
                    tablecell( ss , c->lockState().reportState() );
                    if ( co.active() )
                        tablecell( ss , co.elapsedSeconds() );
                    else
                        tablecell( ss , "" );
                    tablecell( ss , co.getOp() );
                    tablecell( ss , html::escape( co.getNS() ) );
                    if ( co.haveQuery() )
                        tablecell( ss , html::escape( co.query().toString() ) );
                    else
                        tablecell( ss , "" );
                    tablecell( ss , co.getRemoteString() );

                    tablecell( ss , co.getMessage() );
                    tablecell( ss , co.getProgressMeter().toString() );


                    ss << "</tr>\n";
                }
            }
            ss << "</table>\n";

        }

    } clientListPlugin;

    void Client::getReaderWriterClientCount( int *readers , int *writers ) {
        int num = 0;
        int w = 0;
        int r = 0;
        {
            scoped_lock bl(clientsMutex);
            for ( set<Client*>::iterator i=clients.begin(); i!=clients.end(); ++i ) {
                Client* c = *i;
                if ( c->lockState().hasLockPending() ) {
                    num++;
                    if ( c->lockState().hasAnyWriteLock() )
                        w++;
                    else
                        r++;
                }
            }
        }

        verify(writers);
        verify(readers);
        *writers = w;
        *readers = r;
    }

    int Client::getActiveClientCount( int& writers, int& readers ) {
        writers = 0;
        readers = 0;

        scoped_lock bl(clientsMutex);
        for ( set<Client*>::iterator i=clients.begin(); i!=clients.end(); ++i ) {
            Client* c = *i;
            if ( ! c->curop()->active() )
                continue;

            if ( c->lockState().hasAnyWriteLock() )
                writers++;
            if ( c->lockState().hasAnyReadLock() )
                readers++;
        }

        return writers + readers;
    }

    void OpDebug::reset() {
        extra.reset();

        op = 0;
        iscommand = false;
        ns = "";
        query = BSONObj();
        updateobj = BSONObj();

        cursorid = -1;
        ntoreturn = -1;
        ntoskip = -1;
        exhaust = false;

        nscanned = -1;
        idhack = false;
        scanAndOrder = false;
        nupdated = -1;
        nmoved = -1;
        fastmod = false;
        fastmodinsert = false;
        upsert = false;
        keyUpdates = 0;  // unsigned, so -1 not possible
        
        exceptionInfo.reset();
        lockNotGrantedInfo = BSONObj();
        
        executionTime = 0;
        nreturned = -1;
        responseLength = -1;
    }

    bool OpDebug::vetoLog( const CurOp& curop ) const {
        // this is causing many tests to output lots of logs
        // we don't need it.
        // Basically, this is the case of oplog cursors still trying
        // to connect to a machine even though the machine is shutting
        // down. We should find a way for the oplog to veto
        // that machine, but can't find method to do it now
        if (exceptionInfo.code == 11600 && ns == "local.oplog.rs" ) {
            return true;
        }
        return false;
    }


#define OPDEBUG_TOSTRING_HELP(x) if( x >= 0 ) s << " " #x ":" << (x)
#define OPDEBUG_TOSTRING_HELP_BOOL(x) if( x ) s << " " #x ":" << (x)
    string OpDebug::report( const CurOp& curop ) const {
        StringBuilder s;
        if ( iscommand )
            s << "command ";
        else
            s << opToString( op ) << ' ';
        s << ns;

        if ( ! query.isEmpty() ) {
            if ( iscommand )
                s << " command: ";
            else
                s << " query: ";
            s << query.toString(false, true);
        }
        
        if ( ! updateobj.isEmpty() ) {
            s << " update: ";
            updateobj.toString( s );
        }

        OPDEBUG_TOSTRING_HELP( cursorid );
        OPDEBUG_TOSTRING_HELP( ntoreturn );
        OPDEBUG_TOSTRING_HELP( ntoskip );
        OPDEBUG_TOSTRING_HELP_BOOL( exhaust );

        OPDEBUG_TOSTRING_HELP( nscanned );
        OPDEBUG_TOSTRING_HELP_BOOL( idhack );
        OPDEBUG_TOSTRING_HELP_BOOL( scanAndOrder );
        OPDEBUG_TOSTRING_HELP( nmoved );
        OPDEBUG_TOSTRING_HELP( nupdated );
        OPDEBUG_TOSTRING_HELP_BOOL( fastmod );
        OPDEBUG_TOSTRING_HELP_BOOL( fastmodinsert );
        OPDEBUG_TOSTRING_HELP_BOOL( upsert );
        OPDEBUG_TOSTRING_HELP( keyUpdates );
        
        if ( extra.len() )
            s << " " << extra.str();

        if ( ! exceptionInfo.empty() ) {
            s << " exception: " << exceptionInfo.msg;
            if ( exceptionInfo.code )
                s << " code:" << exceptionInfo.code;
        }


        if ( ! lockNotGrantedInfo.isEmpty() ) {
            s << " lockNotGranted: " << lockNotGrantedInfo;
        }

        s << " ";
        curop.lockStat().report( s );
        
        OPDEBUG_TOSTRING_HELP( nreturned );
        if ( responseLength > 0 )
            s << " reslen:" << responseLength;
        s << " " << executionTime << "ms";
        
        return s.str();
    }

#define OPDEBUG_APPEND_NUMBER(x) if( x != -1 ) b.appendNumber( #x , (x) )
#define OPDEBUG_APPEND_BOOL(x) if( x ) b.appendBool( #x , (x) )
    void OpDebug::append( const CurOp& curop, BSONObjBuilder& b ) const {
        b.append( "op" , iscommand ? "command" : opToString( op ) );
        b.append( "ns" , ns );
        if ( ! query.isEmpty() )
            b.append( iscommand ? "command" : "query" , query );
        else if ( ! iscommand && curop.haveQuery() )
            curop.appendQuery( b , "query" );

        if ( ! updateobj.isEmpty() )
            b.append( "updateobj" , updateobj );
        
        const bool moved = (nmoved >= 1);

        OPDEBUG_APPEND_NUMBER( cursorid );
        OPDEBUG_APPEND_NUMBER( ntoreturn );
        OPDEBUG_APPEND_NUMBER( ntoskip );
        OPDEBUG_APPEND_BOOL( exhaust );

        OPDEBUG_APPEND_NUMBER( nscanned );
        OPDEBUG_APPEND_BOOL( idhack );
        OPDEBUG_APPEND_BOOL( scanAndOrder );
        OPDEBUG_APPEND_BOOL( moved );
        OPDEBUG_APPEND_NUMBER( nmoved );
        OPDEBUG_APPEND_NUMBER( nupdated );
        OPDEBUG_APPEND_BOOL( fastmod );
        OPDEBUG_APPEND_BOOL( fastmodinsert );
        OPDEBUG_APPEND_BOOL( upsert );
        OPDEBUG_APPEND_NUMBER( keyUpdates );

        b.append( "lockStats" , curop.lockStat().report() );
        
        if ( ! exceptionInfo.empty() ) 
            exceptionInfo.append( b , "exception" , "exceptionCode" );
        
        OPDEBUG_APPEND_NUMBER( nreturned );
        OPDEBUG_APPEND_NUMBER( responseLength );
        b.append( "millis" , executionTime );
        
    }

}
