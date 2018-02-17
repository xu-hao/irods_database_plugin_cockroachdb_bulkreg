// =-=-=-=-=-=-=-
// irods includes
#include "rodsDef.h"
#include "icatStructs.hpp"
#include "icatHighLevelRoutines.hpp"
#include "mid_level.hpp"
#include "low_level.hpp"

// =-=-=-=-=-=-=-
// new irods includes
#include "irods_database_plugin.hpp"
#include "irods_database_constants.hpp"

// =-=-=-=-=-=-=-
// irods includes
#include "rods.h"
#include "rcMisc.h"
#include "miscServerFunct.hpp"

// =-=-=-=-=-=-=-
// stl includes
#include <sstream>
#include <string>
#include <vector>

#include "irods_api_plugin_bulkreg_constants.hpp"
#include "irods_api_plugin_bulkreg_structs.hpp"
#include <map>

extern icatSessionStruct icss; // JMC :: only for testing!!!
int _rollback(const char*);

template <typename Key, typename Value>
class Cache {
public:
    Cache(std::function<irods::error(const Key &, Value &)> _retrieve) : retrieve_(_retrieve), cache_() { }
    irods::error get(const Key &_key, Value &_value) {
        auto value = cache_.find(_key);
        if(value != cache_.end()) {
            _value = value->second;
            return SUCCESS();
        } else {
            irods::error ret = retrieve_(_key, cache_[_key]);
            if(!ret.ok()) {
                return ret;
            } else {
                _value = cache_[_key];
                return SUCCESS();
            }
        }
    }
private:
    std::map<Key, Value> cache_;
    std::function<irods::error(const Key &, Value &)> retrieve_;
};

irods::error db_bulkreg_op(
    irods::plugin_context& _ctx,
    irods::Bulk*    _inp ) {
    // =-=-=-=-=-=-=-
    // check the context
    irods::error ret = _ctx.valid();
    if ( !ret.ok() ) {
        return PASS( ret );
    }

    // =-=-=-=-=-=-=-
    // check the params
    if ( !_inp ) {
        return ERROR( CAT_INVALID_ARGUMENT, "null parameter" );

    }

    std::function<irods::error()> tx = [&] () {
        
        rodsLog( LOG_NOTICE, "bulkreg started" );
        int status = 0;
        
        
        Cache<std::tuple<std::string, std::string>, rodsLong_t> userCache{[](const std::tuple<std::string, std::string> &_key, rodsLong_t &_value) {
            auto &userName = std::get<0>(_key);
            auto &userZone = std::get<1>(_key);
            std::vector<std::string> bindVars = {userName, userZone};
            rodsLong_t userId;
            int status = cmlGetIntegerValueFromSql("select user_id from r_user_main where user_name = ? and zone_name = ?", &userId, bindVars, &icss);
            if (status < 0) {
                return CODE(status);
            } else {
                _value = userId;
                return SUCCESS();
            }
            }};
        Cache<std::string, rodsLong_t> collCache{[](const std::string&_key, rodsLong_t &_value) {
            auto &collName = _key;
            std::vector<std::string> bindVars = {collName};
            rodsLong_t collId;
            int status = cmlGetIntegerValueFromSql("select coll_id from r_coll_main where coll_name = ?", &collId, bindVars, &icss);
            if (status < 0) {
                return CODE(status);
            } else {
                _value = collId;
                return SUCCESS();
            }
            }};
        Cache<std::string, rodsLong_t> rescCache{[](const std::string&_key, rodsLong_t &_value) {
            auto &rescName = _key;
            std::vector<std::string> bindVars = {rescName};
            rodsLong_t rescId;
            int status = cmlGetIntegerValueFromSql("select resc_id from r_resc_main where resc_name = ?", &rescId, bindVars, &icss);
            if (status < 0) {
                return CODE(status);
            } else {
                _value = rescId;
                return SUCCESS();
            }
            }};
        
        
        std::vector<std::string> sql0 {
            "insert into r_coll_main (coll_id, parent_coll_name, coll_name, coll_owner_name, coll_owner_zone, coll_map_id, coll_inheritance, coll_type, coll_info1, coll_info2, coll_expiry_ts, r_comment, create_ts, modify_ts) values (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            "insert into r_data_main (data_id, coll_id, data_name, data_owner_name, data_owner_zone, data_map_id, data_repl_num, data_version, data_type_name, data_size, data_path, data_is_dirty, data_status, data_checksum, data_expiry_ts, data_mode, r_comment, create_ts, modify_ts, resc_id, resc_name) select ?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?, ?",
            "insert into r_objt_access (object_id, user_id, access_type_id, create_ts, modify_ts) select ?,?,1200,?,?"
        };
        std::vector<std::string> sql;
        
        if(_inp->parallel) {
            std::transform(std::begin(sql0), std::end(sql0), std::back_inserter(sql), [](const std::string &sqlstr) { return sqlstr + " returning nothing"; });
        } else {
            sql = sql0;
        }
        
        for(auto &collection : _inp->collections) {
            auto id = cmlGetNextSeqVal(&icss);
            if(id<0) {
                status = id;
                _rollback("db_bulkreg_op");
                return ERROR( status, "db_bulkreg_op failed" ); 
            }
            std::vector<std::string> bindVars{std::to_string(id), collection.parent_coll_name, collection.coll_name, collection.coll_owner_name, collection.coll_owner_zone,std::to_string(collection.coll_map_id),collection.coll_inheritance,collection.coll_type,collection.coll_info1,collection.coll_info2,collection.coll_expiry_ts,collection.r_comment,collection.create_ts,collection.modify_ts};
            status = cmlExecuteNoAnswerSqlBV(sql[0].c_str(), bindVars, &icss);
            rodsLog( LOG_NOTICE, "bulkreg %s: %d", collection.coll_name.c_str(), status );
            if(status<0 && !(_inp->parallel && status == CAT_SUCCESS_BUT_WITH_NO_INFO)) {
                _rollback("db_bulkreg_op");
                return ERROR( status, "db_bulkreg_op failed" ); 
            }
            
            rodsLong_t userId;
            irods::error ret = userCache.get(std::make_tuple(collection.coll_owner_name, collection.coll_owner_zone), userId);
            if(!ret.ok()) {
                return ret;
            }        
            std::vector<std::string> bindVars2{std::to_string(id), std::to_string(userId), collection.create_ts,collection.create_ts};
            status = cmlExecuteNoAnswerSqlBV(sql[2].c_str(), bindVars2, &icss);
            rodsLog( LOG_NOTICE, "bulkreg access %s: %d", collection.coll_name.c_str(), status );
            if(status<0 && !(_inp->parallel && status == CAT_SUCCESS_BUT_WITH_NO_INFO)) {
                _rollback("db_bulkreg_op");
                return ERROR( status, "db_bulkreg_op failed" ); 
            }
        }
        
        for(auto &data_object : _inp->data_objects) {
            auto id = cmlGetNextSeqVal(&icss);
            if(id<0) {
                status = id;
                _rollback("db_bulkreg_op");
                return ERROR( status, "db_bulkreg_op failed" ); 
            }
            rodsLong_t collId;
            irods::error ret = collCache.get(data_object.parent_coll_name, collId);
            if(!ret.ok()) {
                return ret;
            }        
            rodsLong_t rescId;
            ret = rescCache.get(data_object.resc_name, rescId);
            if(!ret.ok()) {
                return ret;
            }        
            std::vector<std::string> bindVars{std::to_string(id), std::to_string(collId), data_object.data_name, data_object.data_owner_name, data_object.data_owner_zone,std::to_string(data_object.data_map_id), std::to_string(data_object.data_repl_num), data_object.data_version, data_object.data_type_name, std::to_string(data_object.data_size), data_object.data_path, std::to_string(data_object.data_is_dirty), data_object.data_status,  data_object.data_checksum, data_object.data_expiry_ts, data_object.data_mode, data_object.r_comment, data_object.create_ts, data_object.modify_ts, std::to_string(rescId), data_object.resc_name};
            status = cmlExecuteNoAnswerSqlBV(sql[1].c_str(), bindVars, &icss);
            rodsLog( LOG_NOTICE, "bulkreg %s %s: %d", data_object.data_name.c_str(), data_object.parent_coll_name.c_str(), status );
            if(status<0 && !(_inp->parallel && status == CAT_SUCCESS_BUT_WITH_NO_INFO)) {
                _rollback("db_bulkreg_op");
                return ERROR( status, "db_bulkreg_op failed" ); 
            }
            rodsLong_t userId;
            ret = userCache.get(std::make_tuple(data_object.data_owner_name, data_object.data_owner_zone), userId);
            if(!ret.ok()) {
                return ret;
            }        
            std::vector<std::string> bindVars2{std::to_string(id), std::to_string(userId), data_object.create_ts,data_object.create_ts};
            status = cmlExecuteNoAnswerSqlBV(sql[2].c_str(), bindVars2, &icss);
            rodsLog( LOG_NOTICE, "bulkreg access %s %s: %d", data_object.data_name.c_str(), data_object.parent_coll_name.c_str(), status );
            if(status<0 && !(_inp->parallel && status == CAT_SUCCESS_BUT_WITH_NO_INFO)) {
                _rollback("db_bulkreg_op");
                return ERROR( status, "db_bulkreg_op failed" ); 
            }
            
        }
        
        
        rodsLog( LOG_NOTICE, "bulkreg finished" );
        
         return SUCCESS();
         
    };
    
    return execTx(&icss, tx);

} // db_bulkreg_op

extern "C"
irods::database* plugin_functor(
    irods::database* _plugin) {
    _plugin->add_operation<irods::Bulk*>(
	irods::DATABASE_OP_BULKREG,
        std::function<irods::error(irods::plugin_context&,irods::Bulk*)>(
            db_bulkreg_op));
    return _plugin;
}

// =-=-=-=-=-=-=-
// factory function to provide instance of the plugin
extern "C"
irods::database* plugin_factory(
    const std::string& _inst_name,
    const std::string& _context ) {

    irods::database* pg = nullptr;

    irods::error ret = irods::load_plugin<irods::database>(pg, "cockroachdb", irods::PLUGIN_TYPE_DATABASE, _inst_name, _context);

    if(!ret.ok()) {
      rodsLog( LOG_ERROR, "%s", ret.result().c_str() );
      return nullptr;
    }

    return plugin_functor(pg);

} // plugin_factory
