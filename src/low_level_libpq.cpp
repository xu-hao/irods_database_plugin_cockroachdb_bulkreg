/*** Copyright (c), The Regents of the University of California            ***
 *** For more information please refer to files in the COPYRIGHT directory ***/
/*

   These are the Catalog Low Level (cll) routines for talking to postgresql.

   For each of the supported database systems there is .c file like this
   one with a set of routines by the same names.

   Callable functions:
   cllOpenEnv
   cllCloseEnv
   cllConnect
   cllDisconnect
   cllGetRowCount
   cllExecSqlNoResult
   cllExecSqlWithResult
   cllDoneWithResult
   cllDoneWithDefaultResult
   cllGetRow
   cllGetRows
   cllGetNumberOfColumns
   cllGetColumnInfo
   cllNextValueString

   Internal functions are those that do not begin with cll.
   The external functions used are those that begin with SQL.

*/

#include "low_level_libpq.hpp"

#include "irods_log.hpp"
#include "irods_error.hpp"
#include "irods_stacktrace.hpp"
#include "irods_server_properties.hpp"

#include <cctype>
#include <string>
#include <boost/scope_exit.hpp>
#include <boost/algorithm/string.hpp>


int cllBindVarCount = 0;
const char *cllBindVars[MAX_BIND_VARS];
int cllBindVarCountPrev = 0; /* cllBindVarCount earlier in processing */


#define TMP_STR_LEN 1040

#include <stdio.h>
#include <pwd.h>
#include <ctype.h>

#include <vector>
#include <string>
#include <libpq-fe.h>

/*
  call SQLError to get error information and log it
*/
int
logPsgError( int level, PGresult *res ) {
    const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
    const char *psgErrorMsg = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);

    int errorVal = -2;
            if ( strcmp( ( char * )sqlstate, "23505" ) == 0 &&
                    strstr( ( char * )psgErrorMsg, "duplicate key" ) ) {
                errorVal = CATALOG_ALREADY_HAS_ITEM_BY_THAT_NAME;
            }
    
        rodsLog( level, "SQLSTATE: %s", sqlstate );
        rodsLog( level, "SQL Error message: %s", psgErrorMsg );
    
    return errorVal;
}

/*
  Log the bind variables from the global array (after an error)
*/
void
logBindVariables( int level, const std::vector<std::string> &bindVars ) {
    for ( int i = 0; i < bindVars.size(); i++ ) {
        char tmpStr[TMP_STR_LEN + 2];
        snprintf( tmpStr, TMP_STR_LEN, "bindVar[%d]=%s", i + 1, bindVars[i].c_str() );
        rodsLog( level, "%s", tmpStr );
    }
}

result_set::result_set() : res_(nullptr), row_(0) {
}

paging_result_set::paging_result_set(std::function<int(int, int, PGresult *&)> _query, int _offset, int _maxrows) : query_(_query), offset_(_offset), maxrows_(_maxrows) {
}

all_result_set::all_result_set(std::function<int(PGresult *&)> _query) : query_(_query) {
}

result_set::~result_set() {
  clear();
}
 
int paging_result_set::next_row() {
   if(res_ == nullptr || row_ >= PQntuples(res_) - 1) {
     row_ = 0;
     if (res_ != nullptr) {
       offset_ += PQntuples(res_);
     }
     PQclear(res_);
     return query_(offset_, maxrows_, res_);
   } else {
     row_++;
     return 0; 
   }
}
  
int all_result_set::next_row() {
   if(res_ == nullptr) {
     return query_(res_);
   } else if (row_ >= PQntuples(res_) - 1) {
     return CAT_SUCCESS_BUT_WITH_NO_INFO;
   } else {
     row_++;
     return 0; 
   }
}
  
bool result_set::has_row() {
   return res_ != nullptr && PQntuples(res_) > 0; 
}
  
int result_set::row_size() {
   return PQnfields(res_); 
}
  
int result_set::size() {
   return PQntuples(res_); 
}
  
void result_set::get_value(int _col, char *_buf, int _len) {
  snprintf(_buf, _len, "%s", get_value(_col));
}

const char *result_set::get_value(int _col) {
  return PQgetvalue(res_, row_, _col);
}
void result_set::clear() {
  if(res_ != nullptr) {
    PQclear(res_);
    res_ = nullptr;
  }
}

std::string replaceParams(const std::string &_sql) {
  std::stringstream ss;
  int i = 1;
  for(const char &ch : _sql) {
    if (ch == '?') {
      ss << "$" << std::to_string(i++);
    } else {
      ss << ch;
    }
  }
  return ss.str();
}

int _execSql(PGconn *conn, const std::string &_sql, const std::vector<std::string> &bindVars, PGresult *&res) {
      rodsLog( LOG_DEBUG10, "%s", _sql.c_str() );
      rodsLogSql( _sql.c_str() );
      
      std::string sql = replaceParams(_sql);
    
      std::vector<const char *> bs;
      std::transform(bindVars.begin(), bindVars.end(), std::back_inserter(bs), [](const std::string &str){return str.c_str();});
      
      res = PQexecParams( conn, sql.c_str(), bs.size(), NULL, bs.data(), NULL, NULL, 0 );

      ExecStatusType stat = PQresultStatus(res);
      rodsLogSqlResult( PQresStatus(stat) );

      int result = 0;
      if ( stat == PGRES_COMMAND_OK ||
	      stat == PGRES_TUPLES_OK ) {
	  if ( ! boost::iequals( sql, "begin" )  &&
		  ! boost::iequals( sql, "commit" ) &&
		  ! boost::iequals( sql, "rollback" ) ) {
	      if ( atoi(PQcmdTuples(res)) == 0 ) {
		  result = CAT_SUCCESS_BUT_WITH_NO_INFO;
	      }
	  }
      }
      else {
	  logBindVariables( LOG_NOTICE, bindVars );
	  rodsLog( LOG_NOTICE, "_execSql: PQexecParams error: %s sql:%s",
		  PQresStatus(stat), sql.c_str() );
	  result = logPsgError( LOG_NOTICE, res );
	  PQclear(res);
	  res = NULL;
      }

      return result;
}

int
execSql(const icatSessionStruct *icss, result_set **_resset, const std::string &sql, const std::vector<std::string> &bindVars) {
    PGconn *conn = (PGconn *) icss->connectPtr;

    auto resset = new all_result_set([conn, sql, bindVars](PGresult *&res) {
      return _execSql(conn, sql, bindVars, res);
    });
        
    *_resset = resset;
    
    return resset->next_row();
}


int
execSql( const icatSessionStruct *icss, const std::string &sql, const std::vector<std::string> &bindVars) {
    result_set *resset;
    int status = execSql(icss, &resset, sql, bindVars);
    delete resset;
    return status;
}

int
execSql( const icatSessionStruct *icss, result_set **_resset, const std::function<std::string(int, int)> &_sqlgen, const std::vector<std::string> &bindVars, int offset, int maxrows) {

    PGconn *conn = (PGconn *) icss->connectPtr;

    auto resset = new paging_result_set([conn, _sqlgen, bindVars](int offset, int maxrows, PGresult *&res) {
      auto sql = _sqlgen(offset, maxrows);
      return _execSql(conn, sql, bindVars, res);
    }, offset, maxrows);
        
    *_resset = resset;
    
    return resset->next_row();
}

int cllFreeStatement(int _resinx) {
  delete result_sets[_resinx];
  result_sets[_resinx] = nullptr;
  return 0;
}





std::vector<result_set *> result_sets;

static int didBegin = 0;


/*
  Connect to the DBMS.
*/
int
cllConnect( icatSessionStruct *icss, const std::string &host, int port, const std::string &dbname ) {

    PGconn *conn = PQconnectdb(("host=" + host + " port=" + std::to_string(port) + " dbname=" + dbname + " user=" + icss->databaseUsername + " password=" + icss->databasePassword).c_str());

    // =-=-=-=-=-=-=-
    // initialize a connection to the catalog
    ConnStatusType stat = PQstatus(conn);
    if ( stat != CONNECTION_OK ) {
        rodsLog( LOG_ERROR, "cllConnect: SQLConnect failed: %d", stat );
        rodsLog( LOG_ERROR,
                 "cllConnect: SQLConnect failed:host=%s,port=%d,dbname=%s,user=%s,pass=XXXXX\n",
                 host.c_str(),
		 port,
		 dbname.c_str(),
                 icss->databaseUsername );
        rodsLog( LOG_ERROR, "cllConnect: %s \n", PQerrorMessage(conn) );

        PQfinish( conn );
        return -1;
    }

    icss->connectPtr = conn;

    return 0;
}

/*
  Disconnect from the DBMS.
*/
int
cllDisconnect( icatSessionStruct *icss ) {

    PGconn *conn = (PGconn *) icss->connectPtr;
    
    PQfinish(conn);
    
    return 0;
}

/*
  Bind variables from the global array.
*/
int cllGetBindVars(std::vector<std::string> &bindVars) {

    int myBindVarCount = cllBindVarCount;
    cllBindVarCountPrev = cllBindVarCount; /* save in case we need to log error */
    cllBindVarCount = 0; /* reset for next call */

    for ( int i = 0; i < myBindVarCount; ++i ) {
        char tmpStr[TMP_STR_LEN];
        snprintf( tmpStr, sizeof( tmpStr ), "bindVar[%d]=%s", i + 1, cllBindVars[i] );
        rodsLogSql( tmpStr );
        bindVars.push_back(cllBindVars[i]);
    }

    return 0;
}

/*
  Execute a SQL command which has no resulting table.  Examples include
  insert, delete, update, or ddl.
  Insert a 'begin' statement, if necessary.
*/
int
cllExecSqlNoResult( const icatSessionStruct *icss, const char *sql ) {


    if ( strncmp( sql, "commit", 6 ) == 0 ||
            strncmp( sql, "rollback", 8 ) == 0 ) {
        didBegin = 0;
    }
    else {
        if ( didBegin == 0 ) {

            result_set *resset;
            int status = execSql( icss, &resset, "begin");
	    delete resset;
            if ( status != 0 ) {
                return status;
            }
        }
        didBegin = 1;
    }
    std::vector<std::string> bindVars;
    if ( cllGetBindVars( bindVars ) != 0 ) {
	return -1;
    }
    return execSql( icss, sql, bindVars);
}

int find_res_inx() {
  for(int i = 0; i < result_sets.size(); i++) {
    if(result_sets[i] == nullptr) {
      return i;
    }
  }
  int i = result_sets.size();
  result_sets.push_back(nullptr);
  return i;
}

/*
   Execute a SQL command that returns a result table, and
   and bind the default row; and allow optional bind variables.
*/
int
cllExecSqlWithResultBV(
    const icatSessionStruct *icss,
    int *_resinx, 
    const char *sql,
    const std::vector< std::string > &bindVars ) {
  

    *_resinx = find_res_inx();
    
    return execSql(icss, &result_sets[*_resinx], sql, bindVars);    
}

/*
   Execute a SQL command that returns a result table, and
   and bind the default row.
   This version now uses the global array of bind variables.
*/
int
cllExecSqlWithResult( const icatSessionStruct *icss, int *_resinx, const char *sql ) {
    std::vector<std::string> bindVars;
    if ( cllGetBindVars( bindVars ) != 0 ) {
        return -1;
    }
    
    return cllExecSqlWithResultBV(icss, _resinx, sql, bindVars);
}



