// @file  d_split.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include <map>
#include <string>

#include "mongo/pch.h"

#include "mongo/db/cursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/instance.h"
#include "mongo/db/queryoptimizer.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/namespace_details.h"
#include "mongo/client/dbclientcursor.h"

#include "mongo/client/connpool.h"
#include "mongo/client/distlock.h"
#include "mongo/client/remote_transaction.h"
#include "mongo/util/timer.h"

#include "mongo/s/chunk.h" // for static genID only
#include "mongo/s/config.h"
#include "mongo/s/d_logic.h"

namespace mongo {


    class CmdMedianKey : public InformationCommand {
    public:
        CmdMedianKey() : InformationCommand("medianKey") {}
        virtual void help( stringstream &help ) const {
            help << "Deprecated internal command. Use splitVector command instead. \n";
        }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            errmsg = "medianKey command no longer supported. Calling this indicates mismatch between mongo versions.";
            return false;
        }
    } cmdMedianKey;

    class CheckShardingIndex : public QueryCommand {
    public:
        CheckShardingIndex() : QueryCommand("checkShardingIndex") {}
        virtual bool slaveOk() const { return false; }
        virtual void help( stringstream &help ) const {
            help << "Internal command.\n";
        }

        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {

            const char* ns = jsobj.getStringField( "checkShardingIndex" );
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );

            if ( keyPattern.isEmpty() ) {
                errmsg = "no key pattern found in checkShardingindex";
                return false;
            }

            if ( keyPattern.nFields() == 1 && str::equals( "_id" , keyPattern.firstElementFieldName() ) ) {
                result.appendBool( "idskip" , true );
                return true;
            }

            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            if ( min.isEmpty() != max.isEmpty() ) {
                errmsg = "either provide both min and max or leave both empty";
                return false;
            }

            NamespaceDetails *d = nsdetails( ns );
            if ( ! d ) {
                errmsg = "ns not found";
                return false;
            }

            const IndexDetails *idx = d->findIndexByPrefix( keyPattern ,
                                                            true );  /* require single key */
            if ( idx == NULL ) {
                errmsg = "couldn't find valid index for shard key";
                return false;
            }
            // extend min to get (min, MinKey, MinKey, ....)
            min = Helpers::modifiedRangeBound( min , idx->keyPattern() , -1 );
            if  ( max.isEmpty() ) {
                // if max not specified, make it (MaxKey, Maxkey, MaxKey...)
                max = Helpers::modifiedRangeBound( max , idx->keyPattern() , 1 );
            } else {
                // otherwise make it (max,MinKey,MinKey...) so that bound is non-inclusive
                max = Helpers::modifiedRangeBound( max , idx->keyPattern() , -1 );
            }

            shared_ptr<IndexCursor> c( IndexCursor::make( d , *idx , min , max , false , 1 ) );
            auto_ptr<ClientCursor> cc( new ClientCursor( QueryOption_NoCursorTimeout , c , ns ) );
            if ( ! cc->ok() ) {
                // range is empty
                return true;
            }

            // for now, the only check is that all shard keys are filled
            // null is ok, 
            // TODO if $exist for nulls were picking the index, it could be used instead efficiently
            int keyPatternLength = keyPattern.nFields();
            while ( cc->ok() ) {
                BSONObj currKey = c->currKey();
                
                //check that current key contains non-null elements for all fields in keyPattern
                BSONObjIterator i( currKey );
                for( int k = 0; k < keyPatternLength ; k++ ) {
                    if( ! i.more() ) {
                        errmsg = str::stream() << "index key " << currKey
                                               << " too short for pattern " << keyPattern;
                        return false;
                    }
                    BSONElement currKeyElt = i.next();

                    if ( currKeyElt.type() && currKeyElt.type() != jstNULL )
                        continue;

                    BSONObj obj = c->current();
                    BSONObjIterator j( keyPattern );
                    BSONElement real;
                    for ( int x=0; x <= k; x++ )
                        real = j.next();

                    real = obj.getFieldDotted( real.fieldName() );

                    if ( real.type() )
                        continue;
                    
                    ostringstream os;
                    os << "found null value in key " << c->prettyKey( currKey ) << " for doc: "
                       << ( obj.hasField( "_id" ) ? obj.toString() : obj["_id"].toString() );
                    log() << "checkShardingIndex for '" << ns << "' failed: " << os.str() << endl;
                    
                    errmsg = os.str();
                    return false;
                }
                cc->advance();

            }

            return true;
        }
    } cmdCheckShardingIndex;

    class SplitVectorFinder {
        std::exception *_ex;
        NamespaceDetails *_d;
        const IndexDetails &_idx;
        KeyPattern _chunkPattern;
        Ordering _ordering;
        storage::Key _chunkMin, _chunkMax;
        vector<BSONObj> &_splitPoints;
        bool _chunkTooBig;
        bool _doneFindingPoints;
        long long _justSkipped;
        bool _useCursor;
        BSONObj _lastSplitKey;

        void isTooBigCallback(const storage::KeyV1 *endKey, BSONObj *endPK __attribute__((unused)), uint64_t skipped) {
            if (endKey == NULL) {
                return;
            }
            const storage::KeyV1 max(_chunkMax.buf());
            const int c = endKey->woCompare(max, _ordering);
            if (c < 0) {
                _chunkTooBig = true;
            }
        }
        
        void getPointCallback(const storage::KeyV1 *endKey, BSONObj *endPK, uint64_t skipped) {
            if (endKey == NULL) {
                _doneFindingPoints = true;
                return;
            }
            if (skipped == 0) {
                // We didn't actually skip anything, because the current min document is too
                // big.  Fallback to using a cursor.
                _useCursor = true;
                return;
            }

            const storage::KeyV1 max(_chunkMax.buf());
            int c = endKey->woCompare(max, _ordering);
            if (c >= 0) {
                _doneFindingPoints = true;
                return;
            }

            // This wastefully constructs two BSONs when we should be able to go straight from KeyV1
            // format to a BSON with field names.  TODO: optimize it if it shows up in profiling.
            BSONObj splitKey = _chunkPattern.prettyKey(endKey->toBson());
            c = splitKey.woCompare(_lastSplitKey, _ordering);
            if (c < 0) {
                stringstream ss;
                ss << "next split key cannot be less than the last split key. "
                   << "last key: " << _lastSplitKey
                   << "next key: " << splitKey;
                msgasserted(16797, ss.str());
            }
            if (c == 0) {
                // If we got the same as the current chunk min, that means there are many documents
                // with that same key (or a few really big ones).  Since we can't split in the
                // middle of them, we fall back to just using a cursor from this point forward.
                if (!_idx.isIdIndex()) {
                    _chunkMin.reset(*endKey, endPK);
                    _justSkipped += skipped;
                }
                _useCursor = true;
                return;
            }

            // This is our new split key.  We have to save it in storage::Key form for the next
            // query, and in BSONObj form to pass it back.
            _lastSplitKey = splitKey.getOwned();
            _splitPoints.push_back(_lastSplitKey);
            BSONObj modSplitKey = Helpers::modifiedRangeBound(_lastSplitKey, _idx.keyPattern(), -1);
            _chunkMin.reset(modSplitKey, _idx.isIdIndex() ? NULL : &minKey);
        }

        void slowFindSplitPoint(long long targetChunkSize) {
            long long skipped = 0;
            for (shared_ptr<IndexCursor> c(IndexCursor::make(_d, _idx, _chunkMin.key(), _chunkMax.key(), false, 1)); c->ok(); c->advance()) {
                const BSONObj &currKey = c->currKey();
                const BSONObj &currPK = c->currPK();
                long long docsize = currKey.objsize() + currPK.objsize() + c->current().objsize();
                if (skipped + docsize > targetChunkSize) {
                    BSONObj splitKey = _chunkPattern.prettyKey(currKey);
                    int c = splitKey.woCompare(_lastSplitKey, _ordering);
                    massert(16798, "next split key cannot be less than the last split key", c >= 0);
                    if (c > 0) {
                        if (skipped - targetChunkSize > 16<<10) {
                            int logLevel = skipped - targetChunkSize > 512<<10 ? 0 : 1;
                            LOG(logLevel) << "Finding a split point was hard, probably because of high cardinality, on the chunk containing "
                                          << _lastSplitKey << " instead." << endl
                                          << "You should review your choice of shard key." << endl;
                        }
                        _lastSplitKey = splitKey.getOwned();
                        _splitPoints.push_back(_lastSplitKey);
                        BSONObj modSplitKey = Helpers::modifiedRangeBound(_lastSplitKey, _idx.keyPattern(), -1);
                        _chunkMin.reset(modSplitKey, _idx.isIdIndex() ? NULL : &minKey);
                        return;
                    }
                }
                skipped += docsize;
            }
            // If we get all the way to the end, we can't split any more.
            _doneFindingPoints = true;
        }

      public:
        SplitVectorFinder(NamespaceDetails *d, const IndexDetails &idx, const BSONObj &chunkPattern, const BSONObj &min, const BSONObj &max,
                          vector<BSONObj> &splitPoints)
                : _d(d),
                  _idx(idx),
                  _chunkPattern(chunkPattern.getOwned()),
                  _ordering(Ordering::make(_idx.keyPattern())),
                  _chunkMin(min, _idx.isIdIndex() ? NULL : &minKey),
                  _chunkMax(max, _idx.isIdIndex() ? NULL : &maxKey),
                  _splitPoints(splitPoints),
                  _chunkTooBig(false),
                  _doneFindingPoints(false),
                  _justSkipped(0),
                  _useCursor(false),
                  _lastSplitKey()
        {
            massert(16799, "shard key pattern must be a prefix of the index key pattern", chunkPattern.isPrefixOf(_idx.keyPattern()));
        }

        // Functors that wrap the above callbacks
        class IsTooBigCallback {
            SplitVectorFinder &_finder;
          public:
            IsTooBigCallback(SplitVectorFinder &finder) : _finder(finder) {}
            void operator()(const storage::KeyV1 *endKey, BSONObj *endPK, uint64_t skipped) {
                _finder.isTooBigCallback(endKey, endPK, skipped);
            }
        };
        class GetPointCallback {
            SplitVectorFinder &_finder;
          public:
            GetPointCallback(SplitVectorFinder &finder) : _finder(finder) {}
            void operator()(const storage::KeyV1 *endKey, BSONObj *endPK, uint64_t skipped) {
                _finder.getPointCallback(endKey, endPK, skipped);
            }
        };

        // Schedules the calls down into get_key_after_bytes.
        void find(long long maxChunkSize, long long maxSplitPoints) {
            {
                IsTooBigCallback cb(*this);
                _idx.getKeyAfterBytes(_chunkMin, maxChunkSize, cb);
            }
            if (!_chunkTooBig) {
                return;
            }
            {
                // If _chunkMin doesn't actually exist (could be {x: MinKey} for example) we need to
                // get the actual first key in the chunk so that we make sure we don't try to split
                // on the first key.
                shared_ptr<IndexCursor> c(IndexCursor::make(_d, _idx, _chunkMin.key(), _chunkMax.key(), false, 1, 1));
                massert(16794, "didn't find anything actually in our chunk, but we thought we should split it", c->ok());
                _lastSplitKey = _chunkPattern.prettyKey(c->currKey());
            }
            {
                GetPointCallback cb(*this);
                const long long targetChunkSize = maxChunkSize / 2;
                while (!_doneFindingPoints) {
                    if (maxSplitPoints && _splitPoints.size() >= (size_t) maxSplitPoints) {
                        break;
                    }
                    if (_useCursor) {
                        slowFindSplitPoint(targetChunkSize - _justSkipped);
                        _useCursor = false;
                    }
                    else {
                        _idx.getKeyAfterBytes(_chunkMin, targetChunkSize, cb);
                    }
                }
            }
        }
    };

    class SplitVector : public QueryCommand {
    public:
        SplitVector() : QueryCommand("splitVector") {}
        virtual bool slaveOk() const { return false; }
        virtual void help( stringstream &help ) const {
            help <<
                 "Internal command.\n"
                 "examples:\n"
                 "  { splitVector : \"blog.post\" , keyPattern:{x:1} , min:{x:10} , max:{x:20}, maxChunkSize:200 }\n"
                 "  maxChunkSize unit in MBs\n"
                 "  May optionally specify 'maxSplitPoints' to avoid traversing the whole chunk\n"
                 "  \n"
                 "  { splitVector : \"blog.post\" , keyPattern:{x:1} , min:{x:10} , max:{x:20}, force: true }\n"
                 "  'force' will produce one split point even if data is small; defaults to false\n"
                 "NOTE: This command may take a while to run";
        }

        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {

            //
            // 1.a We'll parse the parameters in two steps. First, make sure the we can use the split index to get
            //     a good approximation of the size of the chunk -- without needing to access the actual data.
            //

            const char* ns = jsobj.getStringField( "splitVector" );
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );

            if ( keyPattern.isEmpty() ) {
                errmsg = "no key pattern found in splitVector";
                return false;
            }

            // If min and max are not provided use the "minKey" and "maxKey" for the sharding key pattern.
            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            if ( min.isEmpty() != max.isEmpty() ){
                errmsg = "either provide both min and max or leave both empty";
                return false;
            }

            long long maxSplitPoints = 0;
            BSONElement maxSplitPointsElem = jsobj[ "maxSplitPoints" ];
            if ( maxSplitPointsElem.isNumber() ) {
                maxSplitPoints = maxSplitPointsElem.numberLong();
            }

            vector<BSONObj> splitKeys;

            // Get the size estimate for this namespace
            NamespaceDetails *d = nsdetails( ns );
            if ( ! d ) {
                errmsg = "ns not found";
                return false;
            }

            const IndexDetails *idx = d->findIndexByPrefix( keyPattern ,
                                                            true ); /* require single key */
            if ( idx == NULL ) {
                errmsg = (string)"couldn't find index over splitting key " +
                         keyPattern.clientReadable().toString();
                return false;
            }
            // extend min to get (min, MinKey, MinKey, ....)
            min = Helpers::modifiedRangeBound( min , idx->keyPattern() , -1 );
            if  ( max.isEmpty() ) {
                // if max not specified, make it (MaxKey, Maxkey, MaxKey...)
                max = Helpers::modifiedRangeBound( max , idx->keyPattern() , 1 );
            } else {
                // otherwise make it (max,MinKey,MinKey...) so that bound is non-inclusive
                max = Helpers::modifiedRangeBound( max , idx->keyPattern() , -1 );
            }

            // 'force'-ing a split is equivalent to having maxChunkSize be the size of the current chunk, i.e., the
            // logic below will split that chunk in half
            long long maxChunkSize = 0;
            bool force = false;
            {
                BSONElement maxSizeElem = jsobj[ "maxChunkSize" ];
                BSONElement forceElem = jsobj[ "force" ];

                if ( forceElem.trueValue() ) {
                    force = true;
                }
                else if ( maxSizeElem.isNumber() ) {
                    maxChunkSize = maxSizeElem.numberLong() * 1<<20;
                }
                else {
                    maxSizeElem = jsobj["maxChunkSizeBytes"];
                    if ( maxSizeElem.isNumber() ) {
                        maxChunkSize = maxSizeElem.numberLong();
                    }
                }

                if ( !force && maxChunkSize <= 0 ) {
                    errmsg = "need to specify the desired max chunk size (maxChunkSize or maxChunkSizeBytes)";
                    return false;
                }
            }

            if (!force && idx->clustering()) {
                SplitVectorFinder finder(d, *idx, keyPattern, min, max, splitKeys);
                finder.find(maxChunkSize, maxSplitPoints);
            } else {
                // Haven't implemented a better version using get_key_after_bytes yet, do the slow thing
                NamespaceDetailsAccStats stats;
                BSONObjBuilder statsResult;
                d->fillCollectionStats(&stats, &statsResult, 1);
                const long long recCount = stats.count;
                const long long dataSize = stats.size;

                // 'force'-ing a split is equivalent to having maxChunkSize be the size of the current chunk, i.e., the
                // logic below will split that chunk in half
                if (force) {
                    maxChunkSize = dataSize;
                }

                // If there's not enough data for more than one chunk, no point continuing.
                if ( dataSize < maxChunkSize || recCount == 0 ) {
                    vector<BSONObj> emptyVector;
                    result.append( "splitKeys" , emptyVector );
                    return true;
                }

                log() << "request split points lookup for chunk " << ns << " " << min << " -->> " << max << endl;

                //
                // 2. Traverse the index and count sizes until we meet maxChunkSize, then add that
                //    key to the result vector. If that key appeared in the vector before, we omit
                //    it. The invariant here is that all the instances of a given key value live in
                //    the same chunk.
                //

                Timer timer;
                long long currSize = 0;
                long long currCount = 0;
                long long numChunks = 0;

                {
                    shared_ptr<IndexCursor> c(IndexCursor::make( d , *idx , min , max , false , 1 ));
                    auto_ptr<ClientCursor> cc( new ClientCursor( QueryOption_NoCursorTimeout , c , ns ) );
                    if ( ! cc->ok() ) {
                        errmsg = "can't open a cursor for splitting (desired range is possibly empty)";
                        return false;
                    }

                    // Use every 'keyCount'-th key as a split point. We add the initial key as a sentinel, to be removed
                    // at the end. If a key appears more times than entries allowed on a chunk, we issue a warning and
                    // split on the following key.
                    set<BSONObj> tooFrequentKeys;
                    splitKeys.push_back(c->prettyKey(c->currKey().getOwned()).extractFields(keyPattern));
                    while (true) {
                        while (cc->ok()) {
                            const BSONObj &currObj = cc->current();
                            currSize += currObj.objsize();
                            currCount++;

                            // we want ~half-full chunks
                            if (2 * currSize >= maxChunkSize) {
                                BSONObj currKey = c->prettyKey(c->currKey()).extractFields(keyPattern);
                                // Do not use this split key if it is the same used in the previous split point.
                                if (currKey.woCompare(splitKeys.back()) == 0) {
                                    tooFrequentKeys.insert(currKey.getOwned());
                                }
                                else {
                                    splitKeys.push_back(currKey.getOwned());
                                    currCount = 0;
                                    currSize = 0;
                                    numChunks++;

                                    LOG(4) << "picked a split key: " << currKey << endl;
                                }
                            }

                            cc->advance();

                            // Stop if we have enough split points.
                            if (maxSplitPoints && (numChunks >= maxSplitPoints)) {
                                log() << "max number of requested split points reached (" << numChunks
                                      << ") before the end of chunk " << ns << " " << min << " -->> " << max
                                      << endl;
                                break;
                            }
                        }

                        if (splitKeys.size() > 1 || !force) {
                            break;
                        }

                        force = false;
                        maxChunkSize = currSize;
                        currCount = 0;
                        LOG(0) << "splitVector doing another cycle because of force, maxChunkSize now: " << maxChunkSize << endl;

                        c = IndexCursor::make(d, *idx, min, max, false, 1);
                        cc.reset(new ClientCursor(QueryOption_NoCursorTimeout, c, ns));
                    }

                    //
                    // 3. Format the result and issue any warnings about the data we gathered while traversing the
                    //    index
                    //

                    // Warn for keys that are more numerous than maxChunkSize allows.
                    for ( set<BSONObj>::const_iterator it = tooFrequentKeys.begin(); it != tooFrequentKeys.end(); ++it ) {
                        warning() << "chunk is larger than " << maxChunkSize
                                  << " bytes because of key " << c->prettyKey( *it ) << endl;
                    }

                    // Remove the sentinel at the beginning before returning
                    splitKeys.erase( splitKeys.begin() );
                    verify( c.get() );

                    if ( timer.millis() > cmdLine.slowMS ) {
                        warning() << "Finding the split vector for " <<  ns << " over "<< keyPattern
                                  << " maxChunkSize: " << maxChunkSize << " numSplits: " << splitKeys.size() 
                                  << " lookedAt: " << currCount << " took " << timer.millis() << "ms"
                                  << endl;
                    }
                }
            }

            // Warning: we are sending back an array of keys but are currently limited to
            // 4MB work of 'result' size. This should be okay for now.

            result.append( "splitKeys" , splitKeys );

            return true;
        }
    } cmdSplitVector;

    // ** temporary ** 2010-10-22
    // chunkInfo is a helper to collect and log information about the chunks generated in splitChunk.
    // It should hold the chunk state for this module only, while we don't have min/max key info per chunk on the
    // mongod side. Do not build on this; it will go away.
    struct ChunkInfo {
        BSONObj min;
        BSONObj max;
        ShardChunkVersion lastmod;

        ChunkInfo() { }
        ChunkInfo( BSONObj aMin , BSONObj aMax , ShardChunkVersion aVersion ) : min(aMin) , max(aMax) , lastmod(aVersion) {}
        void appendShortVersion( const char* name, BSONObjBuilder& b ) const;
        string toString() const;
    };

    void ChunkInfo::appendShortVersion( const char * name , BSONObjBuilder& b ) const {
        BSONObjBuilder bb( b.subobjStart( name ) );
        bb.append( "min" , min );
        bb.append( "max" , max );
        lastmod.addToBSON( bb, "lastmod" );
        bb.done();
    }

    string ChunkInfo::toString() const {
        ostringstream os;
        os << "lastmod: " << lastmod.toString() << " min: " << min << " max: " << endl;
        return os.str();
    }
    // ** end temporary **

    class SplitChunkCommand : public Command {
    public:
        SplitChunkCommand() : Command( "splitChunk" ) {}
        virtual void help( stringstream& help ) const {
            help <<
                 "internal command usage only\n"
                 "example:\n"
                 " { splitChunk:\"db.foo\" , keyPattern: {a:1} , min : {a:100} , max: {a:200} { splitKeys : [ {a:150} , ... ]}";
        }

        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return OPLOCK; }
        virtual bool requiresSync() const { return false; }
        virtual bool needsTxn() const { return false; }
        virtual int txnFlags() const { return noTxnFlags(); }
        virtual bool canRunInMultiStmtTxn() const { return false; }
        virtual OpSettings getOpSettings() const { return OpSettings(); }

        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {

            //
            // 1. check whether parameters passed to splitChunk are sound
            //

            const string ns = cmdObj.firstElement().str();
            if ( ns.empty() ) {
                errmsg  = "need to specify namespace in command";
                return false;
            }

            const BSONObj keyPattern = cmdObj["keyPattern"].Obj();
            if ( keyPattern.isEmpty() ) {
                errmsg = "need to specify the key pattern the collection is sharded over";
                return false;
            }

            const BSONObj min = cmdObj["min"].Obj();
            if ( min.isEmpty() ) {
                errmsg = "need to specify the min key for the chunk";
                return false;
            }

            const BSONObj max = cmdObj["max"].Obj();
            if ( max.isEmpty() ) {
                errmsg = "need to specify the max key for the chunk";
                return false;
            }

            const string from = cmdObj["from"].str();
            if ( from.empty() ) {
                errmsg = "need specify server to split chunk at";
                return false;
            }

            const BSONObj splitKeysElem = cmdObj["splitKeys"].Obj();
            if ( splitKeysElem.isEmpty() ) {
                errmsg = "need to provide the split points to chunk over";
                return false;
            }
            vector<BSONObj> splitKeys;
            BSONObjIterator it( splitKeysElem );
            while ( it.more() ) {
                splitKeys.push_back( it.next().Obj().getOwned() );
            }

            const BSONElement shardId = cmdObj["shardId"];
            if ( shardId.eoo() ) {
                errmsg = "need to provide shardId";
                return false;
            }

            // It is possible that this is the first sharded command this mongod is asked to perform. If so,
            // start sharding apparatus. We'd still be missing some more shard-related info but we'll get it
            // in step 2. below.
            if ( ! shardingState.enabled() ) {
                if ( cmdObj["configdb"].type() != String ) {
                    errmsg = "sharding not enabled";
                    return false;
                }
                string configdb = cmdObj["configdb"].String();
                ShardingState::initialize(configdb);
            }

            Shard myShard( from );

            log() << "received splitChunk request: " << cmdObj << endl;

            //
            // 2. lock the collection's metadata and get highest version for the current shard
            //

            DistributedLock lockSetup( ConnectionString( shardingState.getConfigServer() , ConnectionString::SYNC) , ns );
            dist_lock_try dlk;

            try{
            	dlk = dist_lock_try( &lockSetup, string("split-") + min.toString() );
            }
            catch( LockException& e ){
            	errmsg = str::stream() << "Error locking distributed lock for split." << causedBy( e );
            	return false;
            }

            if ( ! dlk.got() ) {
                errmsg = "the collection's metadata lock is taken";
                result.append( "who" , dlk.other() );
                return false;
            }

            // TODO This is a check migrate does to the letter. Factor it out and share. 2010-10-22

            ShardChunkVersion maxVersion;
            string shard;
            ChunkInfo origChunk;
            {
                scoped_ptr<ScopedDbConnection> conn(
                        ScopedDbConnection::getInternalScopedDbConnection(
                                shardingState.getConfigServer() ) );

                BSONObj x = conn->get()->findOne( ShardNS::chunk,
                                                  Query( BSON( "ns" << ns ) )
                                                      .sort( BSON( "lastmod" << -1 ) ) );
                maxVersion = ShardChunkVersion::fromBSON( x, "lastmod" );

                BSONObj currChunk = conn->get()->findOne( ShardNS::chunk , shardId.wrap( "_id" ) )
                    .getOwned();
                verify( currChunk["shard"].type() );
                verify( currChunk["min"].type() );
                verify( currChunk["max"].type() );
                shard = currChunk["shard"].String();
                conn->done();

                BSONObj currMin = currChunk["min"].Obj();
                BSONObj currMax = currChunk["max"].Obj();
                if ( currMin.woCompare( min ) || currMax.woCompare( max ) ) {
                    errmsg = "chunk boundaries are outdated (likely a split occurred)";
                    result.append( "currMin" , currMin );
                    result.append( "currMax" , currMax );
                    result.append( "requestedMin" , min );
                    result.append( "requestedMax" , max );

                    LOG( LL_WARNING ) << "aborted split because " << errmsg << ": " << min << "->" << max
                                      << " is now " << currMin << "->" << currMax << endl;
                    return false;
                }

                if ( shard != myShard.getName() ) {
                    errmsg = "location is outdated (likely balance or migrate occurred)";
                    result.append( "from" , myShard.getName() );
                    result.append( "official" , shard );

                    LOG( LL_WARNING ) << "aborted split because " << errmsg << ": chunk is at " << shard
                                      << " and not at " << myShard.getName() << endl;
                    return false;
                }

                if ( maxVersion < shardingState.getVersion( ns ) ) {
                    errmsg = "official version less than mine?";
                    maxVersion.addToBSON( result, "officialVersion" );
                    shardingState.getVersion( ns ).addToBSON( result, "myVersion" );

                    LOG( LL_WARNING ) << "aborted split because " << errmsg << ": official " << maxVersion
                                      << " mine: " << shardingState.getVersion(ns) << endl;
                    return false;
                }

                origChunk.min = currMin.getOwned();
                origChunk.max = currMax.getOwned();
                origChunk.lastmod = ShardChunkVersion::fromBSON( currChunk["lastmod"] );

                // since this could be the first call that enable sharding we also make sure to have the chunk manager up to date
                shardingState.gotShardName( shard );
                ShardChunkVersion shardVersion;
                shardingState.trySetVersion( ns , shardVersion /* will return updated */ );

                log() << "splitChunk accepted at version " << shardVersion << endl;

            }

            //
            // 3. Update the metadata ( the new chunks ) in a transaction
            //

            BSONObjBuilder logDetail;
            origChunk.appendShortVersion( "before" , logDetail );
            LOG(1) << "before split on " << origChunk << endl;
            vector<ChunkInfo> newChunks;

            try {
                scoped_ptr<ScopedDbConnection> conn(ScopedDbConnection::getInternalScopedDbConnection(shardingState.getConfigServer()));
                RemoteTransaction txn(conn->conn(), "serializable");

                // Check the precondition
                BSONObjBuilder b;
                b.appendTimestamp("lastmod", maxVersion.toLong());
                BSONObj expect = b.obj();
                Matcher m(expect);

                BSONObj found = conn->get()->findOne(ShardNS::chunk, QUERY("ns" << ns).sort("lastmod", -1));
                if (!m.matches(found)) {
                    // TODO(leif): Make sure that this means the sharding algorithm is broken and we should bounce the server.
                    error() << "splitChunk commit failed: " << ShardChunkVersion::fromBSON(found["lastmod"])
                            << " instead of " << maxVersion << endl;
                    error() << "TERMINATING" << endl;
                    dbexit(EXIT_SHARDING_ERROR);
                }

                ShardChunkVersion myVersion = maxVersion;
                BSONObj startKey = min;
                splitKeys.push_back( max ); // makes it easier to have 'max' in the next loop. remove later.

                for ( vector<BSONObj>::const_iterator it = splitKeys.begin(); it != splitKeys.end(); ++it ) {
                    BSONObj endKey = *it;

                    // splits only update the 'minor' portion of version
                    myVersion.incMinor();

                    try {
                        BSONObjBuilder n;
                        n.append( "_id" , Chunk::genID( ns , startKey ) );
                        myVersion.addToBSON( n, "lastmod" );
                        n.append( "ns" , ns );
                        n.append( "min" , startKey );
                        n.append( "max" , endKey );
                        n.append( "shard" , shard );
                        conn->get()->update(ShardNS::chunk, QUERY("_id" << Chunk::genID(ns, startKey)), n.obj(), true);
                    }
                    catch (DBException &e) {
                        warning() << e << endl;
                        error() << "splitChunk error updating the chunk ending in " << endKey << endl;
                        throw e;
                    }

                    // remember this chunk info for logging later
                    newChunks.push_back( ChunkInfo( startKey , endKey, myVersion ) );

                    startKey = endKey;
                }

                splitKeys.pop_back(); // 'max' was used as sentinel

                txn.commit();
                conn->done();
            }
            catch (DBException &e) {
                stringstream ss;
                ss << "saving chunks failed.  reason: " << e.what();
                error() << ss.str() << endl;
                msgasserted( 13593 , ss.str() );
            }

            // install a chunk manager with knowledge about newly split chunks in this shard's state
            maxVersion.incMinor();
            shardingState.splitChunk( ns , min , max , splitKeys , maxVersion );

            //
            // 5. logChanges
            //

            // single splits are logged different than multisplits
            if ( newChunks.size() == 2 ) {
                newChunks[0].appendShortVersion( "left" , logDetail );
                newChunks[1].appendShortVersion( "right" , logDetail );
                configServer.logChange( "split" , ns , logDetail.obj() );

            }
            else {
                BSONObj beforeDetailObj = logDetail.obj();
                BSONObj firstDetailObj = beforeDetailObj.getOwned();
                const int newChunksSize = newChunks.size();

                for ( int i=0; i < newChunksSize; i++ ) {
                    BSONObjBuilder chunkDetail;
                    chunkDetail.appendElements( beforeDetailObj );
                    chunkDetail.append( "number", i+1 );
                    chunkDetail.append( "of" , newChunksSize );
                    newChunks[i].appendShortVersion( "chunk" , chunkDetail );
                    configServer.logChange( "multi-split" , ns , chunkDetail.obj() );
                }
            }

            if (newChunks.size() == 2){
                Client::ReadContext ctx( ns );
                Client::Transaction txn(DB_TXN_SNAPSHOT | DB_TXN_READ_ONLY);
                // If one of the chunks has only one object in it we should move it
                for (int i=1; i >= 0 ; i--){ // high chunk more likely to have only one obj

                    NamespaceDetails *d = nsdetails( ns.c_str() );

                    const IndexDetails *idx = d->findIndexByPrefix( keyPattern ,
                                                                    true ); /* exclude multikeys */
                    if ( idx == NULL ) {
                        break;
                    }

                    ChunkInfo chunk = newChunks[i];
                    BSONObj newmin = Helpers::modifiedRangeBound(chunk.min, idx->keyPattern(), -1);
                    BSONObj newmax = Helpers::modifiedRangeBound(chunk.max , idx->keyPattern(), -1);

                    shared_ptr<Cursor> c( IndexCursor::make( d , *idx ,
                                                             newmin , /* lower */
                                                             newmax , /* upper */
                                                             false , /* upper noninclusive */
                                                             1 ) ); /* direction */

                    // check if exactly one document found
                    if ( c->ok() ) {
                        c->advance();
                        if ( c->eof() ) {
                            result.append( "shouldMigrate",
                                           BSON("min" << chunk.min << "max" << chunk.max) );
                            break;
                        }
                    }
                }
                txn.commit();
            }

            return true;
        }
    } cmdSplitChunk;

}  // namespace mongo
