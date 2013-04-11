/**
*    Copyright (C) 2012 Tokutek Inc.
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

#include "mongo/pch.h"
#include "oplog_helpers.h"
#include "txn_context.h"
#include "repl_block.h"
#include "stats/counters.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/insert.h"


#define KEY_STR_OP_NAME "op"
#define KEY_STR_NS "ns"
#define KEY_STR_ROW "o"
#define KEY_STR_OLD_ROW "o"
#define KEY_STR_NEW_ROW "o2"
#define KEY_STR_COMMENT "o"
#define KEY_STR_MIGRATE "fromMigrate"

// values for types of operations in opLog
#define OP_STR_INSERT "i"
#define OP_STR_UPDATE "u"
#define OP_STR_DELETE "d"
#define OP_STR_COMMENT "n"
#define OP_STR_COMMAND "c"

namespace mongo {
namespace OpLogHelpers{

    static inline void appendOpType(const char *opstr, BSONObjBuilder* b) {
        b->append(KEY_STR_OP_NAME, opstr);
    }
    static inline void appendNsStr(const char *ns, BSONObjBuilder* b) {
        b->append(KEY_STR_NS, ns);
    }
    static inline void appendMigrate(bool fromMigrate, BSONObjBuilder* b) {
        if (fromMigrate) {
            b->append(KEY_STR_MIGRATE, true);
        }
    }

    static inline bool isLocalNs(const char* ns) {
        return (strncmp(ns, "local.", 6) == 0);
        // TODO: (Zardosht) figure out what resetSlaveCache does and when we should call it
    }
    
    void logComment(BSONObj comment, TxnContext* txn) {
        if (logTxnOperations()) {
            BSONObjBuilder b;
            appendOpType(OP_STR_COMMENT, &b);
            b.append(KEY_STR_COMMENT, comment);
            txn->logOp(b.obj());
        }
    }
    
    void logInsert(const char* ns, BSONObj row, TxnContext* txn) {
        if (logTxnOperations()) {
            BSONObjBuilder b;
            if ( strncmp(ns, "local.slaves", 12) == 0 ) {
              resetSlaveCache();
            }
            if (isLocalNs(ns)) {
                return;
            }

            appendOpType(OP_STR_INSERT, &b);
            appendNsStr(ns, &b);
            b.append(KEY_STR_ROW, row);
            txn->logOp(b.obj());
        }
    }

    void logUpdate(
        const char* ns, 
        BSONObj oldRow, 
        BSONObj newRow,
        bool fromMigrate,
        TxnContext* txn
        ) 
    {
        if (logTxnOperations()) {
            BSONObjBuilder b;
            if ( strncmp(ns, "local.slaves", 12) == 0 ) {
              resetSlaveCache();
            }
            if (isLocalNs(ns)) {
                return;
            }

            appendOpType(OP_STR_UPDATE, &b);
            appendNsStr(ns, &b);
            appendMigrate(fromMigrate, &b);
            b.append(KEY_STR_OLD_ROW, oldRow);
            b.append(KEY_STR_NEW_ROW, newRow);
            txn->logOp(b.obj());
        }
    }

    void logDelete(const char* ns, BSONObj row, bool fromMigrate, TxnContext* txn) {
        if (logTxnOperations()) {
            BSONObjBuilder b;
            if ( strncmp(ns, "local.slaves", 12) == 0 ) {
              resetSlaveCache();
            }
            if (isLocalNs(ns)) {
                return;
            }

            appendOpType(OP_STR_DELETE, &b);
            appendNsStr(ns, &b);
            appendMigrate(fromMigrate, &b);
            b.append(KEY_STR_ROW, row);
            txn->logOp(b.obj());
        }
    }

    void logCommand(const char* ns, BSONObj row, TxnContext* txn) {
        if (logTxnOperations()) {
            BSONObjBuilder b;
            if ( strncmp(ns, "local.slaves", 12) == 0 ) {
              resetSlaveCache();
            }
            if (isLocalNs(ns)) {
                return;
            }

            appendOpType(OP_STR_COMMAND, &b);
            appendNsStr(ns, &b);
            b.append(KEY_STR_ROW, row);
            txn->logOp(b.obj());
        }
    }

    static void runInsertFromOplog(const char* ns, BSONObj op) {
        NamespaceDetails* nsd = nsdetails(ns);
        NamespaceDetailsTransient *nsdt = &NamespaceDetailsTransient::get(ns);
        BSONObj row = op[KEY_STR_ROW].Obj();
        // handle add index case
        if (mongoutils::str::endsWith(ns, ".system.indexes")) {
            BSONObj key = row["key"].Obj();
            int i = nsd->findIndexByKeyPattern(key);
            if (i >= 0) {
                uasserted(16475, "index exists on secondary");
            } else {
                nsd->createIndex(row);
            }
        }
        // overwrite set to true because we are running on a secondary
        insertOneObject(nsd, nsdt, row, true);
    }

    static void runDeleteFromOplog(const char* ns, BSONObj op) {
        NamespaceDetails* nsd = nsdetails(ns);
        NamespaceDetailsTransient *nsdt = &NamespaceDetailsTransient::get(ns);
        BSONObj row = op[KEY_STR_ROW].Obj();
        BSONObj pk = row["_id"].wrap("");
        deleteOneObject(nsd, nsdt, pk, row);
    }

    static void runUpdateFromOplog(const char* ns, BSONObj op) {
        NamespaceDetails* nsd = nsdetails(ns);
        NamespaceDetailsTransient *nsdt = &NamespaceDetailsTransient::get(ns);
        const char *names[] = { 
            KEY_STR_OLD_ROW, 
            KEY_STR_OLD_ROW
            };
        BSONElement fields[2];
        op.getFields(2, names, fields);
        BSONObj oldRow = fields[0].Obj();
        BSONObj newRow = fields[1].Obj();
        BSONObj pk = oldRow["_id"].wrap("");
        struct LogOpUpdateDetails loud;
        loud.logop = false;
        loud.ns = NULL;
        loud.fromMigrate = false;
        updateOneObject(nsd, nsdt, pk, oldRow, newRow, &loud);
    }

    static void runCommandFromOplog(const char* ns, BSONObj op) {
        BufBuilder bb;
        BSONObjBuilder ob;
        BSONObj command = op[KEY_STR_ROW].embeddedObject();
        _runCommands(ns, command, bb, ob, true, 0);
    }

    void applyOperationFromOplog(const BSONObj& op) {
        LOG(6) << "applying op: " << op << endl;
        OpCounters* opCounters = &replOpCounters;
        const char *names[] = { 
            KEY_STR_NS, 
            KEY_STR_OP_NAME
            };
        BSONElement fields[2];
        op.getFields(2, names, fields);
        const char* ns = fields[0].valuestrsafe();
        const char* opType = fields[1].valuestrsafe();
        if (strcmp(opType, OP_STR_INSERT)) {
            opCounters->gotInsert();
            runInsertFromOplog(ns, op);
        }
        else if (strcmp(opType, OP_STR_UPDATE)) {
            opCounters->gotUpdate();
            runUpdateFromOplog(ns, op);
        }
        else if (strcmp(opType, OP_STR_DELETE)) {
            opCounters->gotDelete();
            runDeleteFromOplog(ns, op);
        }
        else if (strcmp(opType, OP_STR_COMMAND)) {
            opCounters->gotCommand();
            runCommandFromOplog(ns, op);
        }
        else if (strcmp(opType, OP_STR_COMMENT)) {
            // no-op
        }
        else {
            throw MsgAssertionException( 14825 , ErrorMsg("error in applyOperation : unknown opType ", *opType) );
        }
    }
} // namespace OpLogHelpers
} // namespace mongo