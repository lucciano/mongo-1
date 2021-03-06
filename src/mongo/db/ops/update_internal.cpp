//@file update_internal.cpp

/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "pch.h"

#include "mongo/db/oplog.h"
#include "mongo/db/jsobjmanipulator.h"
#include "mongo/util/mongoutils/str.h"

#include "update_internal.h"

//#define DEBUGUPDATE(x) cout << x << endl;
#define DEBUGUPDATE(x)

namespace mongo {

    const char* Mod::modNames[] = { "$inc", "$set", "$push", "$pushAll", "$pull", "$pullAll" , "$pop", "$unset" ,
                                    "$bitand" , "$bitor" , "$bit" , "$addToSet", "$rename", "$rename"
                                  };
    unsigned Mod::modNamesNum = sizeof(Mod::modNames)/sizeof(char*);

    bool Mod::_pullElementMatch( BSONElement& toMatch ) const {

        if ( elt.type() != Object ) {
            // if elt isn't an object, then comparison will work
            return toMatch.woCompare( elt , false ) == 0;
        }

        if ( matcherOnPrimitive )
            return matcher->matches( toMatch.wrap( "" ) );

        if ( toMatch.type() != Object ) {
            // looking for an object, so this can't match
            return false;
        }

        // now we have an object on both sides
        return matcher->matches( toMatch.embeddedObject() );
    }

    void Mod::appendIncremented( BSONBuilderBase& builder , const BSONElement& in, ModState& ms ) const {
        BSONType a = in.type();
        BSONType b = elt.type();

        if ( a == NumberDouble || b == NumberDouble ) {
            ms.incType = NumberDouble;
            ms.incdouble = elt.numberDouble() + in.numberDouble();
        }
        else if ( a == NumberLong || b == NumberLong ) {
            ms.incType = NumberLong;
            ms.inclong = elt.numberLong() + in.numberLong();
        }
        else {
            int x = elt.numberInt() + in.numberInt();
            if ( x < 0 && elt.numberInt() > 0 && in.numberInt() > 0 ) {
                // overflow
                ms.incType = NumberLong;
                ms.inclong = elt.numberLong() + in.numberLong();
            }
            else {
                ms.incType = NumberInt;
                ms.incint = elt.numberInt() + in.numberInt();
            }
        }

        ms.appendIncValue( builder , false );
    }

    void appendUnset( BSONBuilderBase& builder ) {
        if ( builder.isArray() ) {
            builder.appendNull();
        }
    }

    void Mod::apply( BSONBuilderBase& builder , BSONElement in , ModState& ms ) const {
        if ( ms.dontApply ) {
            // Pass the original element through unchanged.
            builder << in;
            return;
        }

        switch ( op ) {

        case INC: {
            appendIncremented( builder , in , ms );
            // We don't need to "fix" this operation into a $set, for oplog purposes,
            // here. ModState::appendForOpLog will do that for us. It relies on the new value
            // being in inc{int,long,double} inside the ModState that wraps around this Mod.
            break;
        }

        case SET: {
            _checkForAppending( elt );
            builder.appendAs( elt , shortFieldName );
            break;
        }

        case UNSET: {
            appendUnset( builder );
            break;
        }

        case PUSH: {
            uassert( 10131 ,  "$push can only be applied to an array" , in.type() == Array );
            BSONArrayBuilder bb( builder.subarrayStart( shortFieldName ) );
            BSONObjIterator i( in.embeddedObject() );
            while ( i.more() ) {
                bb.append( i.next() );
            }

            bb.append( elt );

            // We don't want to log a positional $set for which the '_checkForAppending' test
            // won't pass. If we're in that case, fall back to non-optimized logging.
            if ( (elt.type() == Object && elt.embeddedObject().okForStorage()) ||
                 (elt.type() != Object) ) {
                ms.fixedOpName = "$set";
                ms.forcePositional = true;
                ms.position = bb.arrSize() - 1;
                bb.done();
            }
            else {
                ms.fixedOpName = "$set";
                ms.forceEmptyArray = true;
                ms.fixedArray = BSONArray( bb.done().getOwned() );
            }

            break;
        }

        case ADDTOSET: {
            uassert( 12592 ,  "$addToSet can only be applied to an array" , in.type() == Array );
            BSONArrayBuilder bb( builder.subarrayStart( shortFieldName ) );
            BSONObjIterator i( in.embeddedObject() );

            if ( isEach() ) {

                BSONElementSet toadd;
                parseEach( toadd );

                while ( i.more() ) {
                    BSONElement cur = i.next();
                    bb.append( cur );
                    toadd.erase( cur );
                }

                {
                    BSONObjIterator i( getEach() );
                    while ( i.more() ) {
                        BSONElement e = i.next();
                        if ( toadd.count(e) ) {
                            bb.append( e );
                            toadd.erase( e );
                        }
                    }
                }

                ms.fixedOpName = "$set";
                ms.forceEmptyArray = true;
                ms.fixedArray = BSONArray(bb.done().getOwned());
            }
            else {

                bool found = false;
                int pos = 0;
                int count = 0;
                while ( i.more() ) {
                    BSONElement cur = i.next();
                    bb.append( cur );
                    if ( elt.woCompare( cur , false ) == 0 ) {
                        found = true;
                        pos = count;
                    }
                    count++;
                }

                if ( !found ) {
                    bb.append( elt );
                }

                // We don't want to log a positional $set for which the '_checkForAppending'
                // test won't pass. If we're in that case, fall back to non-optimized logging.
                if ( (elt.type() == Object && elt.embeddedObject().okForStorage()) ||
                     (elt.type() != Object) ) {
                    ms.fixedOpName = "$set";
                    ms.forcePositional = true;
                    ms.position = found ? pos : bb.arrSize() - 1;
                    bb.done();
                }
                else {
                    ms.fixedOpName = "$set";
                    ms.forceEmptyArray = true;
                    ms.fixedArray = BSONArray(bb.done().getOwned());
                }
            }

            break;
        }

        case PUSH_ALL: {
            uassert( 10132 ,  "$pushAll can only be applied to an array" , in.type() == Array );
            uassert( 10133 ,  "$pushAll has to be passed an array" , elt.type() );

            BSONArrayBuilder bb( builder.subarrayStart( shortFieldName ) );

            BSONObjIterator i( in.embeddedObject() );
            while ( i.more() ) {
                bb.append( i.next() );
            }

            i = BSONObjIterator( elt.embeddedObject() );
            while ( i.more() ) {
                bb.append( i.next() );
            }

            ms.fixedOpName = "$set";
            ms.forceEmptyArray = true;
            ms.fixedArray = BSONArray(bb.done().getOwned());
            break;
        }

        case PULL:
        case PULL_ALL: {
            uassert( 10134 ,  "$pull/$pullAll can only be applied to an array" , in.type() == Array );
            BSONArrayBuilder bb( builder.subarrayStart( shortFieldName ) );

            //temporarily record the things to pull. only use this set while 'elt' in scope.
            BSONElementSet toPull;
            if ( op == PULL_ALL ) {
                BSONObjIterator j( elt.embeddedObject() );
                while ( j.more() ) {
                    toPull.insert( j.next() );
                }
            }

            BSONObjIterator i( in.embeddedObject() );
            while ( i.more() ) {
                BSONElement e = i.next();
                bool allowed = true;

                if ( op == PULL ) {
                    allowed = ! _pullElementMatch( e );
                }
                else {
                    allowed = ( toPull.find( e ) == toPull.end() );
                }

                if ( allowed )
                    bb.append( e );
            }

            // If this is the last element of the array, then we want to write the empty array to the
            // oplog.
            ms.fixedOpName = "$set";
            ms.forceEmptyArray = true;
            ms.fixedArray = BSONArray(bb.done().getOwned());
            break;
        }

        case POP: {
            uassert( 10135 ,  "$pop can only be applied to an array" , in.type() == Array );
            BSONArrayBuilder bb( builder.subarrayStart( shortFieldName ) );


            BSONObjIterator i( in.embeddedObject() );
            if ( elt.isNumber() && elt.number() < 0 ) {
                // pop from front
                if ( i.more() ) {
                    i.next();
                }

                while( i.more() ) {
                    bb.append( i.next() );
                }
            }
            else {
                // pop from back
                while( i.more() ) {
                    BSONElement arrI = i.next();
                    if ( i.more() ) {
                        bb.append( arrI );
                    }
                }
            }

            ms.fixedOpName = "$set";
            ms.forceEmptyArray = true;
            ms.fixedArray = BSONArray(bb.done().getOwned());
            break;
        }

        case BIT: {
            uassert( 10136 ,  "$bit needs an object" , elt.type() == Object );
            uassert( 10137 ,  "$bit can only be applied to numbers" , in.isNumber() );
            uassert( 10138 ,  "$bit cannot update a value of type double" , in.type() != NumberDouble );

            int x = in.numberInt();
            long long y = in.numberLong();

            BSONObjIterator it( elt.embeddedObject() );
            while ( it.more() ) {
                BSONElement e = it.next();
                uassert( 10139 ,  "$bit field must be number" , e.isNumber() );
                if ( str::equals(e.fieldName(), "and") ) {
                    switch( in.type() ) {
                    case NumberInt: x = x&e.numberInt(); break;
                    case NumberLong: y = y&e.numberLong(); break;
                    default: verify( 0 );
                    }
                }
                else if ( str::equals(e.fieldName(), "or") ) {
                    switch( in.type() ) {
                    case NumberInt: x = x|e.numberInt(); break;
                    case NumberLong: y = y|e.numberLong(); break;
                    default: verify( 0 );
                    }
                }
                else {
                    uasserted(9016, str::stream() << "unknown $bit operation: " << e.fieldName());
                }
            }

            switch( in.type() ) {

            case NumberInt:
                builder.append( shortFieldName , x );
                // By recording the result of the bit manipulation into the ModSet, we'll be
                // set up so that this $bit operation be "fixed" as a $set of the final result
                // in the oplog. This will happen in appendForOpLog and what triggers it is
                // setting the incType in the ModSet that is around this Mod.
                ms.incType = NumberInt;
                ms.incint = x;
                break;

            case NumberLong:
                // Please see comment on fixing this $bit into a $set for logging purposes in
                // the NumberInt case.
                builder.append( shortFieldName , y );
                ms.incType = NumberLong;
                ms.inclong = y;
                break;

            default: verify( 0 );
            }

            break;
        }

        case RENAME_FROM: {
            // We don't need to "fix" this operation into a $set here. ModState::appendForOpLog
            // will do that for us. It relies on the field name being stored on this Mod.
            break;
        }

        case RENAME_TO: {
            // We don't need to "fix" this operation into a $set here, for the same reason we
            // didn't either with RENAME_FROM.
            ms.handleRename( builder, shortFieldName );
            break;
        }

        default:
            uasserted( 9017 , str::stream() << "Mod::apply can't handle type: " << op );
        }
    }

    // -1 inside a non-object (non-object could be array)
    // 0 missing
    // 1 found
    int validRenamePath( BSONObj obj, const char* path ) {
        while( const char* p = strchr( path, '.' ) ) {
            string left( path, p - path );
            BSONElement e = obj.getField( left );
            if ( e.eoo() ) {
                return 0;
            }
            if ( e.type() != Object ) {
                return -1;
            }
            obj = e.embeddedObject();
            path = p + 1;
        }
        return !obj.getField( path ).eoo();
    }

    auto_ptr<ModSetState> ModSet::prepare(const BSONObj& obj) const {
        DEBUGUPDATE( "\t start prepare" );
        auto_ptr<ModSetState> mss( new ModSetState( obj ) );


        // Perform this check first, so that we don't leave a partially modified object on uassert.
        for ( ModHolder::const_iterator i = _mods.begin(); i != _mods.end(); ++i ) {
            DEBUGUPDATE( "\t\t prepare : " << i->first );
            mss->_mods[i->first].reset( new ModState() );
            ModState& ms = *mss->_mods[i->first];

            const Mod& m = i->second;
            BSONElement e = obj.getFieldDotted(m.fieldName);

            ms.m = &m;
            ms.old = e;

            if ( m.op == Mod::RENAME_FROM ) {
                int source = validRenamePath( obj, m.fieldName );
                uassert( 13489, "$rename source field invalid", source != -1 );
                if ( source != 1 ) {
                    ms.dontApply = true;
                }
                continue;
            }

            if ( m.op == Mod::RENAME_TO ) {
                int source = validRenamePath( obj, m.renameFrom() );
                if ( source == 1 ) {
                    int target = validRenamePath( obj, m.fieldName );
                    uassert( 13490, "$rename target field invalid", target != -1 );
                    ms.newVal = obj.getFieldDotted( m.renameFrom() );
                }
                else {
                    ms.dontApply = true;
                }
                continue;
            }

            if ( e.eoo() ) {
                continue;
            }

            switch( m.op ) {
            case Mod::INC:
                uassert( 10140 ,  "Cannot apply $inc modifier to non-number", e.isNumber() || e.eoo() );
                break;

            default:
            case Mod::SET:
                break;

            case Mod::PUSH:
            case Mod::PUSH_ALL:
                uassert( 10141,
                         "Cannot apply $push/$pushAll modifier to non-array",
                         e.type() == Array || e.eoo() );
                break;

            case Mod::PULL:
            case Mod::PULL_ALL:
                uassert( 10142,
                         "Cannot apply $pull/$pullAll modifier to non-array",
                         e.type() == Array || e.eoo() );
                break;

            case Mod::POP:
                uassert( 10143,
                         "Cannot apply $pop modifier to non-array",
                         e.type() == Array || e.eoo() );
                break;

            case Mod::ADDTOSET:
                uassert( 12591,
                         "Cannot apply $addToSet modifier to non-array",
                         e.type() == Array || e.eoo() );
                break;
            }
        }

        DEBUGUPDATE( "\t mss\n" << mss->toString() << "\t--" );

        return mss;
    }

    const char* ModState::getOpLogName() const {
        if ( dontApply ) {
            return NULL;
        }

        if ( incType ) {
            return "$set";
        }

        if ( m->op == Mod::RENAME_FROM ) {
            return "$unset";
        }

        if ( m->op == Mod::RENAME_TO ) {
            return "$set";
        }

        return fixedOpName ? fixedOpName : Mod::modNames[op()];
    }


    void ModState::appendForOpLog( BSONObjBuilder& bb ) const {
        // dontApply logic is deprecated for all but $rename.
        if ( dontApply ) {
            return;
        }

        if ( incType ) {
            DEBUGUPDATE( "\t\t\t\t\t appendForOpLog inc fieldname: " << m->fieldName
                         << " short:" << m->shortFieldName );
            appendIncValue( bb , true );
            return;
        }

        if ( m->op == Mod::RENAME_FROM ) {
            DEBUGUPDATE( "\t\t\t\t\t appendForOpLog RENAME_FROM fieldName:" << m->fieldName );
            bb.append( m->fieldName, 1 );
            return;
        }

        if ( m->op == Mod::RENAME_TO ) {
            DEBUGUPDATE( "\t\t\t\t\t appendForOpLog RENAME_TO fieldName:" << m->fieldName );
            bb.appendAs( newVal, m->fieldName );
            return;
        }

        const char* name = fixedOpName ? fixedOpName : Mod::modNames[op()];

        DEBUGUPDATE( "\t\t\t\t\t appendForOpLog name:" << name << " fixed: " << fixed
                     << " fn: " << m->fieldName );

        if (strcmp(name, "$unset") == 0) {
            bb.append(m->fieldName, 1);
            return;
        }

        if ( fixed ) {
            bb.appendAs( *fixed , m->fieldName );
        }
        else if ( ! fixedArray.isEmpty() || forceEmptyArray ) {
            bb.append( m->fieldName, fixedArray );
        }
        else if ( forcePositional ) {
            string positionalField = str::stream() << m->fieldName << "." << position;
            bb.appendAs( m->elt, positionalField.c_str() );
        }
        else {
            bb.appendAs( m->elt , m->fieldName );
        }

    }

    typedef map<string, vector<ModState*> > NamedModMap;

    BSONObj ModSetState::getOpLogRewrite() const {
        NamedModMap names;
        for ( ModStateHolder::const_iterator i = _mods.begin(); i != _mods.end(); ++i ) {
            const char* name = i->second->getOpLogName();
            if ( ! name )
                continue;
            names[name].push_back( i->second.get() );
        }

        BSONObjBuilder b;
        for ( NamedModMap::const_iterator i = names.begin();
              i != names.end();
              ++i ) {
            BSONObjBuilder bb( b.subobjStart( i->first ) );
            const vector<ModState*>& mods = i->second;
            for ( unsigned j = 0; j < mods.size(); j++ ) {
                mods[j]->appendForOpLog( bb );
            }
            bb.doneFast();
        }
        return b.obj();
    }

    string ModState::toString() const {
        stringstream ss;
        if ( fixedOpName )
            ss << " fixedOpName: " << fixedOpName;
        if ( fixed )
            ss << " fixed: " << fixed;
        return ss.str();
    }

    void ModState::handleRename( BSONBuilderBase& newObjBuilder, const char* shortFieldName ) {
        newObjBuilder.appendAs( newVal , shortFieldName );
        BSONObjBuilder b;
        b.appendAs( newVal, shortFieldName );
        verify( _objData.isEmpty() );
        _objData = b.obj();
        newVal = _objData.firstElement();
    }

    void ModSetState::_appendNewFromMods( const string& root,
                                          ModState& modState,
                                          BSONBuilderBase& builder,
                                          set<string>& onedownseen ) {
        Mod& m = *((Mod*)(modState.m)); // HACK
        switch (m.op) {
        // unset/pull/pullAll on nothing does nothing, so don't append anything. Still,
        // explicitly log that the target array was reset.
        case Mod::POP:
        case Mod::PULL:
        case Mod::PULL_ALL:
        case Mod::UNSET:
            modState.fixedOpName = "$unset";
            return;

        // $rename may involve dotted path creation, so we want to make sure we're not
        // creating a path here for a rename that's a no-op. In other words if we're
        // issuing a {$rename: {a.b : c.d} } that's a no-op, we don't want to create
        // the a and c paths here. See test NestedNoName in the 'repl' suite.
        case Mod::RENAME_FROM:
        case Mod::RENAME_TO:
            if (modState.dontApply) {
                return;
            }

        default:
            ;// fall through
        }
        const char*  temp = modState.fieldName();
        temp += root.size();
        const char* dot = strchr( temp , '.' );
        if ( dot ) {
            string nr( modState.fieldName() , 0 , 1 + ( dot - modState.fieldName() ) );
            string nf( temp , 0 , dot - temp );
            if ( onedownseen.count( nf ) )
                return;
            onedownseen.insert( nf );
            BSONObjBuilder bb ( builder.subobjStart( nf ) );
            // Always insert an object, even if the field name is numeric.
            createNewObjFromMods( nr , bb , BSONObj() );
            bb.done();
        }
        else {
            appendNewFromMod( modState , builder );
        }
    }

    bool ModSetState::duplicateFieldName( const BSONElement& a, const BSONElement& b ) {
        return
        !a.eoo() &&
        !b.eoo() &&
        ( a.rawdata() != b.rawdata() ) &&
        str::equals( a.fieldName(), b.fieldName() );
    }

    ModSetState::ModStateRange ModSetState::modsForRoot( const string& root ) {
        ModStateHolder::iterator mstart = _mods.lower_bound( root );
        StringBuilder buf;
        buf << root << (char)255;
        ModStateHolder::iterator mend = _mods.lower_bound( buf.str() );
        return make_pair( mstart, mend );
    }

    void ModSetState::createNewObjFromMods( const string& root,
                                            BSONObjBuilder& builder,
                                            const BSONObj& obj ) {
        BSONObjIteratorSorted es( obj );
        createNewFromMods( root, builder, es, modsForRoot( root ), LexNumCmp( true ) );
    }

    void ModSetState::createNewArrayFromMods( const string& root,
                                              BSONArrayBuilder& builder,
                                              const BSONArray& arr ) {
        BSONArrayIteratorSorted es( arr );
        ModStateRange objectOrderedRange = modsForRoot( root );
        ModStateHolder arrayOrderedMods( LexNumCmp( false ) );
        arrayOrderedMods.insert( objectOrderedRange.first, objectOrderedRange.second );
        ModStateRange arrayOrderedRange( arrayOrderedMods.begin(), arrayOrderedMods.end() );
        createNewFromMods( root, builder, es, arrayOrderedRange, LexNumCmp( false ) );
    }

    void ModSetState::createNewFromMods( const string& root,
                                         BSONBuilderBase& builder,
                                         BSONIteratorSorted& es,
                                         const ModStateRange& modRange,
                                         const LexNumCmp& lexNumCmp ) {

        DEBUGUPDATE( "\t\t createNewFromMods root: " << root );
        ModStateHolder::iterator m = modRange.first;
        const ModStateHolder::const_iterator mend = modRange.second;
        BSONElement e = es.next();

        set<string> onedownseen;
        BSONElement prevE;
        while ( !e.eoo() && m != mend ) {

            if ( duplicateFieldName( prevE, e ) ) {
                // Just copy through an element with a duplicate field name.
                builder.append( e );
                prevE = e;
                e = es.next();
                continue;
            }
            prevE = e;

            string field = root + e.fieldName();
            FieldCompareResult cmp = compareDottedFieldNames( m->second->m->fieldName , field ,
                                                             lexNumCmp );

            DEBUGUPDATE( "\t\t\t field:" << field << "\t mod:"
                         << m->second->m->fieldName << "\t cmp:" << cmp
                         << "\t short: " << e.fieldName() );

            switch ( cmp ) {

            case LEFT_SUBFIELD: { // Mod is embedded under this element

                // SERVER-4781
                bool isObjOrArr = e.type() == Object || e.type() == Array;
                if ( ! isObjOrArr ) {
                    if (m->second->m->strictApply) {
                        uasserted( 10145,
                                   str::stream() << "LEFT_SUBFIELD only supports Object: " << field
                                   << " not: " << e.type() );
                    }
                    else {
                        // Since we're not applying the mod, we keep what was there before
                        builder.append( e );

                        // Skip both as we're not applying this mod. Note that we'll advance
                        // the iterator on the mod side for all the mods that are under the
                        // root we are now.
                        e = es.next();
                        m++;
                        while ( m != mend &&
                                ( compareDottedFieldNames( m->second->m->fieldName,
                                                           field,
                                                           lexNumCmp ) == LEFT_SUBFIELD ) ) {
                            m++;
                        }
                        continue;
                    }
                }

                if ( onedownseen.count( e.fieldName() ) == 0 ) {
                    onedownseen.insert( e.fieldName() );
                    if ( e.type() == Object ) {
                        BSONObjBuilder bb( builder.subobjStart( e.fieldName() ) );
                        stringstream nr; nr << root << e.fieldName() << ".";
                        createNewObjFromMods( nr.str() , bb , e.Obj() );
                        bb.done();
                    }
                    else {
                        BSONArrayBuilder ba( builder.subarrayStart( e.fieldName() ) );
                        stringstream nr; nr << root << e.fieldName() << ".";
                        createNewArrayFromMods( nr.str() , ba , BSONArray( e.embeddedObject() ) );
                        ba.done();
                    }
                    // inc both as we handled both
                    e = es.next();
                    m++;
                    while ( m != mend &&
                            ( compareDottedFieldNames( m->second->m->fieldName , field , lexNumCmp ) ==
                              LEFT_SUBFIELD ) ) {
                        m++;
                    }
                }
                else {
                    massert( 16069 , "ModSet::createNewFromMods - "
                            "SERVER-4777 unhandled duplicate field" , 0 );
                }
                continue;
            }
            case LEFT_BEFORE: // Mod on a field that doesn't exist
                DEBUGUPDATE( "\t\t\t\t creating new field for: " << m->second->m->fieldName );
                _appendNewFromMods( root , *m->second , builder , onedownseen );
                m++;
                continue;
            case SAME:
                DEBUGUPDATE( "\t\t\t\t applying mod on: " << m->second->m->fieldName );
                m->second->apply( builder , e );
                e = es.next();
                m++;
                continue;
            case RIGHT_BEFORE: // field that doesn't have a MOD
                DEBUGUPDATE( "\t\t\t\t just copying" );
                builder.append( e ); // if array, ignore field name
                e = es.next();
                continue;
            case RIGHT_SUBFIELD:
                massert( 10399 ,  "ModSet::createNewFromMods - RIGHT_SUBFIELD should be impossible" , 0 );
                break;
            default:
                massert( 10400 ,  "unhandled case" , 0 );
            }
        }

        // finished looping the mods, just adding the rest of the elements
        while ( !e.eoo() ) {
            DEBUGUPDATE( "\t\t\t copying: " << e.fieldName() );
            builder.append( e );  // if array, ignore field name
            e = es.next();
        }

        // do mods that don't have fields already
        for ( ; m != mend; m++ ) {
            DEBUGUPDATE( "\t\t\t\t appending from mod at end: " << m->second->m->fieldName );
            _appendNewFromMods( root , *m->second , builder , onedownseen );
        }
    }

    BSONObj ModSetState::createNewFromMods() {
        BSONObjBuilder b( (int)(_obj.objsize() * 1.1) );
        createNewObjFromMods( "" , b , _obj );
        return _newFromMods = b.obj();
    }

    string ModSetState::toString() const {
        stringstream ss;
        for ( ModStateHolder::const_iterator i=_mods.begin(); i!=_mods.end(); ++i ) {
            ss << "\t\t" << i->first << "\t" << i->second->toString() << "\n";
        }
        return ss.str();
    }

    BSONObj ModSet::createNewFromQuery( const BSONObj& query ) {
        BSONObj newObj;

        {
            BSONObjBuilder bb;
            EmbeddedBuilder eb( &bb );
            BSONObjIteratorSorted i( query );
            while ( i.more() ) {
                BSONElement e = i.next();
                if ( e.fieldName()[0] == '$' ) // for any operators we add
                    continue;

                if ( e.type() == Object && e.embeddedObject().firstElementFieldName()[0] == '$' ) {
                    // we have something like { x : { $gt : 5 } }
                    // this can be a query piece
                    // or can be a dbref or something

                    int op = e.embeddedObject().firstElement().getGtLtOp();
                    if ( op > 0 ) {
                        // This means this is a $gt type filter, so don't make it part of the new
                        // object.
                        continue;
                    }

                    if ( str::equals( e.embeddedObject().firstElement().fieldName(), "$not" ) ) {
                        // A $not filter operator is not detected in getGtLtOp() and should not
                        // become part of the new object.
                        continue;
                    }
                }

                eb.appendAs( e , e.fieldName() );
            }
            eb.done();
            newObj = bb.obj();
        }

        auto_ptr<ModSetState> mss = prepare( newObj );
        newObj = mss->createNewFromMods();

        return newObj;
    }

    /* get special operations like $inc
       { $inc: { a:1, b:1 } }
       { $set: { a:77 } }
       { $push: { a:55 } }
       { $pushAll: { a:[77,88] } }
       { $pull: { a:66 } }
       { $pullAll : { a:[99,1010] } }
       NOTE: MODIFIES source from object!
    */
    ModSet::ModSet(
        const BSONObj& from ,
        const set<string>& idxKeys,
        const set<string>* backgroundKeys,
        bool forReplication)
        : _isIndexed(0) , _hasDynamicArray( false ) {

        BSONObjIterator it(from);

        while ( it.more() ) {
            BSONElement e = it.next();
            const char* fn = e.fieldName();

            uassert( 10147 ,  "Invalid modifier specified: " + string( fn ), e.type() == Object );
            BSONObj j = e.embeddedObject();
            DEBUGUPDATE( "\t" << j );

            BSONObjIterator jt(j);
            Mod::Op op = opFromStr( fn );

            while ( jt.more() ) {
                BSONElement f = jt.next(); // x:44

                const char* fieldName = f.fieldName();

                // Allow remove of invalid field name in case it was inserted before this check
                // was added (~ version 2.1).
                uassert( 15896,
                         "Modified field name may not start with $",
                         fieldName[0] != '$' || op == Mod::UNSET );
                uassert( 10148,
                         "Mod on _id not allowed",
                         strcmp( fieldName, "_id" ) != 0 );
                uassert( 10149,
                         "Invalid mod field name, may not end in a period",
                         fieldName[ strlen( fieldName ) - 1 ] != '.' );
                uassert( 10150,
                         "Field name duplication not allowed with modifiers",
                         ! haveModForField( fieldName ) );
                uassert( 10151,
                         "have conflicting mods in update",
                         ! haveConflictingMod( fieldName ) );
                uassert( 10152,
                         "Modifier $inc allowed for numbers only",
                         f.isNumber() || op != Mod::INC );
                uassert( 10153,
                         "Modifier $pushAll/pullAll allowed for arrays only",
                         f.type() == Array || ( op != Mod::PUSH_ALL && op != Mod::PULL_ALL ) );

                if ( op == Mod::RENAME_TO ) {
                    uassert( 13494, "$rename target must be a string", f.type() == String );
                    const char* target = f.valuestr();
                    uassert( 13495,
                             "$rename source must differ from target",
                             strcmp( fieldName, target ) != 0 );
                    uassert( 13496,
                             "invalid mod field name, source may not be empty",
                             fieldName[0] );
                    uassert( 13479,
                             "invalid mod field name, target may not be empty",
                             target[0] );
                    uassert( 13480,
                             "invalid mod field name, source may not begin or end in period",
                             fieldName[0] != '.' && fieldName[ strlen( fieldName ) - 1 ] != '.' );
                    uassert( 13481,
                             "invalid mod field name, target may not begin or end in period",
                             target[0] != '.' && target[ strlen( target ) - 1 ] != '.' );
                    uassert( 13482,
                             "$rename affecting _id not allowed",
                             !( fieldName[0] == '_' && fieldName[1] == 'i' && fieldName[2] == 'd'
                                && ( !fieldName[3] || fieldName[3] == '.' ) ) );
                    uassert( 13483,
                             "$rename affecting _id not allowed",
                             !( target[0] == '_' && target[1] == 'i' && target[2] == 'd'
                                && ( !target[3] || target[3] == '.' ) ) );
                    uassert( 13484,
                             "field name duplication not allowed with $rename target",
                             !haveModForField( target ) );
                    uassert( 13485,
                             "conflicting mods not allowed with $rename target",
                             !haveConflictingMod( target ) );
                    uassert( 13486,
                             "$rename target may not be a parent of source",
                             !( strncmp( fieldName, target, strlen( target ) ) == 0
                                && fieldName[ strlen( target ) ] == '.' ) );
                    uassert( 13487,
                             "$rename source may not be dynamic array",
                             strstr( fieldName , ".$" ) == 0 );
                    uassert( 13488,
                             "$rename target may not be dynamic array",
                             strstr( target , ".$" ) == 0 );

                    Mod from;
                    from.init( Mod::RENAME_FROM, f , forReplication );
                    from.setFieldName( fieldName );
                    updateIsIndexed( from, idxKeys, backgroundKeys );
                    _mods[ from.fieldName ] = from;

                    Mod to;
                    to.init( Mod::RENAME_TO, f , forReplication );
                    to.setFieldName( target );
                    updateIsIndexed( to, idxKeys, backgroundKeys );
                    _mods[ to.fieldName ] = to;

                    DEBUGUPDATE( "\t\t " << fieldName << "\t" << from.fieldName << "\t" << to.fieldName );
                    continue;
                }

                _hasDynamicArray = _hasDynamicArray || strstr( fieldName , ".$" ) > 0;

                Mod m;
                m.init( op , f , forReplication );
                m.setFieldName( f.fieldName() );
                updateIsIndexed( m, idxKeys, backgroundKeys );
                _mods[m.fieldName] = m;

                DEBUGUPDATE( "\t\t " << fieldName << "\t" << m.fieldName << "\t" << _hasDynamicArray );
            }
        }

    }

    ModSet* ModSet::fixDynamicArray( const string& elemMatchKey ) const {
        ModSet* n = new ModSet();
        n->_isIndexed = _isIndexed;
        n->_hasDynamicArray = _hasDynamicArray;
        for ( ModHolder::const_iterator i=_mods.begin(); i!=_mods.end(); i++ ) {
            string s = i->first;
            size_t idx = s.find( ".$" );
            if ( idx == string::npos ) {
                n->_mods[s] = i->second;
                continue;
            }
            StringBuilder buf;
            buf << s.substr(0,idx+1) << elemMatchKey << s.substr(idx+2);
            string fixed = buf.str();
            DEBUGUPDATE( "fixed dynamic: " << s << " -->> " << fixed );
            n->_mods[fixed] = i->second;
            ModHolder::iterator temp = n->_mods.find( fixed );
            temp->second.setFieldName( temp->first.c_str() );
        }
        return n;
    }

    void ModSet::updateIsIndexed( const set<string>& idxKeys, const set<string>* backgroundKeys ) {
        for ( ModHolder::const_iterator i = _mods.begin(); i != _mods.end(); ++i )
            updateIsIndexed( i->second, idxKeys , backgroundKeys );
    }

    bool getCanonicalIndexField( const string& fullName, string* out ) {
        // check if fieldName contains ".$" or ".###" substrings (#=digit) and skip them
        if ( fullName.find( '.' ) == string::npos )
            return false;

        bool modified = false;

        StringBuilder buf;
        for ( size_t i=0; i<fullName.size(); i++ ) {

            char c = fullName[i];

            if ( c != '.' ) {
                buf << c;
                continue;
            }

            // check for ".$", skip if present
            if ( fullName[i+1] == '$' ) {
                i++;
                modified = true;
                continue;
            }

            // check for ".###" for any number of digits (no letters)
            if ( isdigit( fullName[i+1] ) ) {
                size_t j = i;
                // skip digits
                while ( j+1 < fullName.size() && isdigit( fullName[j+1] ) )
                    j++;

                if ( j+1 == fullName.size() || fullName[j+1] == '.' ) {
                    // only digits found, skip forward
                    i = j;
                    modified = true;
                    continue;
                }
            }

            buf << c;
        }

        if ( !modified )
            return false;

        *out = buf.str();
        return true;
    }


} // namespace mongo
