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

#include "low_level_odbc.hpp"

#include "irods_log.hpp"
#include "irods_error.hpp"
#include "irods_stacktrace.hpp"
#include "irods_server_properties.hpp"

#include <cctype>
#include <string>
#include <boost/scope_exit.hpp>

int _cllFreeStatementColumns( icatSessionStruct *icss, int statementNumber );

int
_cllExecSqlNoResult( icatSessionStruct *icss, const char *sql, int option );


int cllBindVarCount = 0;
const char *cllBindVars[MAX_BIND_VARS];
int cllBindVarCountPrev = 0; /* cllBindVarCount earlier in processing */

const static SQLLEN GLOBAL_SQL_NTS = SQL_NTS;

/* Different argument types are needed on at least Ubuntu 11.04 on a
   64-bit host when using MySQL, but may or may not apply to all
   64-bit hosts.  The ODBCVER in sql.h is the same, 0x0351, but some
   of the defines differ.  If it's using new defines and this isn't
   used, there may be compiler warnings but it might link OK, but not
   operate correctly consistently. */
#define SQL_INT_OR_LEN SQLLEN
#define SQL_UINT_OR_ULEN SQLULEN

/* for now: */
#define MAX_TOKEN 256

#define TMP_STR_LEN 1040

SQLINTEGER columnLength[MAX_TOKEN];  /* change me ! */

#include <stdio.h>
#include <pwd.h>
#include <ctype.h>

#include <vector>
#include <string>
#include <libpq-fe.h>

static int didBegin = 0;

// =-=-=-=-=-=-=-
// JMC :: Needed to add this due to crash issues with the SQLBindCol + SQLFetch
//     :: combination where the fetch fails if a var is not passed to the bind for
//     :: the result data size
static const short MAX_NUMBER_ICAT_COLUMS = 32;
static SQLLEN resultDataSizeArray[ MAX_NUMBER_ICAT_COLUMS ];


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
                 host,
		 port,
		 dbname,
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
  Execute a SQL command which has no resulting table.  Examples include
  insert, delete, update, or ddl.
  Insert a 'begin' statement, if necessary.
*/
int
cllExecSqlNoResult( icatSessionStruct *icss, const char *sql ) {

    if ( strncmp( sql, "commit", 6 ) == 0 ||
            strncmp( sql, "rollback", 8 ) == 0 ) {
        didBegin = 0;
    }
    else {
        if ( didBegin == 0 ) {
            int status = _cllExecSqlNoResult( icss, "begin", 1 );
            if ( status != PGRES_COMMAND_OK ) {
                return status;
            }
        }
        didBegin = 1;
    }
    return _cllExecSqlNoResult( icss, sql, 0 );
}

/*
  Log the bind variables from the global array (after an error)
*/
void
logTheBindVariables( int level ) {
    for ( int i = 0; i < cllBindVarCountPrev; i++ ) {
        char tmpStr[TMP_STR_LEN + 2];
        snprintf( tmpStr, TMP_STR_LEN, "bindVar[%d]=%s", i + 1, cllBindVars[i] );
        rodsLog( level, "%s", tmpStr );
    }
}

/*
  Bind variables from the global array.
*/
int
bindTheVariables( std::vector<const char *> &bindVars ) {

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
  Execute a SQL command which has no resulting table.  With optional
  bind variables.
  If option is 1, skip the bind variables.
*/
int
_cllExecSqlNoResult(
    icatSessionStruct* icss,
    const char*        sql,
    int                option ) {
    rodsLog( LOG_DEBUG10, "%s", sql );

    PGconn *conn = (PGconn *) icss->connectPtr;

    std::vector<const char *> bindVars;
    if ( option == 0 && bindTheVariables( bindVars ) != 0 ) {
        return -1;
    }

    rodsLogSql( sql );

    PGresult *res = PQexecParams(conn, sql, bindVars.size(), NULL, bindVars.data(), NULL, NULL, 0);
    
    BOOST_SCOPE_EXIT(&res) {
      PQclear(res);
    } BOOST_SCOPE_EXIT_END
    
    ResultStatusType stat = PQresultStatus(res);
    rodsLogSqlResult( PQresStatus(stat) );

    int result;
    if ( stat == PGRES_COMMAND_OK ||
            stat == PGRES_TUPLES_OK ) {
        result = 0;
        if ( ! cmp_stmt( sql, "begin" )  &&
                ! cmp_stmt( sql, "commit" ) &&
                ! cmp_stmt( sql, "rollback" ) ) {
            if ( atoi(PQcmdTuples(res)) == 0 ) {
                result = CAT_SUCCESS_BUT_WITH_NO_INFO;
            }
            if ( stat == PGRES_TUPLES_OK && PQntuples(res) == 0 ) {
                result = CAT_SUCCESS_BUT_WITH_NO_INFO;
            }
        }
    }
    else {
        if ( option == 0 ) {
            logTheBindVariables( LOG_NOTICE );
        }
        rodsLog( LOG_NOTICE, "_cllExecSqlNoResult: SQLExecDirect error: %d sql:%s",
                 PQresStatus(stat), sql );
        result = logPsgError( LOG_NOTICE, res );
    }

    return result;
}

/*
   Execute a SQL command that returns a result table, and
   and bind the default row.
   This version now uses the global array of bind variables.
*/
int
_cllExecSqlWithResult( icatSessionStruct *icss, PGresult *&res, const char *sql, std::vector<const char *> bindVars ) {


    rodsLog( LOG_DEBUG10, "%s", sql );

    PGconn *conn = (PGconn *) icss->connectPtr;

    res = NULL;

    std::vector<const char *> bindVars;
    if ( bindTheVariables( bindVars ) != 0 ) {
        return -1;
    }

    rodsLogSql( sql );
    res = PQexecParams( conn, sql, bindVars.size(), NULL, bindVars.data(), NULL, NULL, 0 );

    ResultStatusType stat = PQresultStatus(res);
    rodsLogSqlResult( PQresStatus(stat) );

    if ( stat != PGRES_COMMAND_OK &&
            stat == PGRES_TUPLES_OK ) {
        logTheBindVariables( LOG_NOTICE );
        rodsLog( LOG_NOTICE,
                 "cllExecSqlWithResult: SQLExecDirect error: %d, sql:%s",
                 stat, sql );
        logPsgError( LOG_NOTICE, res );
	PQclear(res);
	res = NULL;
        return -1;
    }

    return 0;
}

int
cllExecSqlWithResult( icatSessionStruct *icss, PGresult *&res, const char *sql ) {
    res = NULL;

    std::vector<const char *> bindVars;
    if ( bindTheVariables( bindVars ) != 0 ) {
        return -1;
    }
    return _cllExecSqlWithResult(icss, res, sql, bindVars);
}
/* logBindVars
   For when an error occurs, log the bind variables which were used
   with the sql.
*/
void
logBindVars(
    int level,
    std::vector<std::string> &bindVars ) {
    for ( std::size_t i = 0; i < bindVars.size(); i++ ) {
        if ( !bindVars[i].empty() ) {
            rodsLog( level, "bindVar%d=%s", i + 1, bindVars[i].c_str() );
        }
    }
}


/*
   Execute a SQL command that returns a result table, and
   and bind the default row; and allow optional bind variables.
*/
int
cllExecSqlWithResultBV(
    icatSessionStruct *icss,
    PGresult *&res,
    const char *sql,
    std::vector< std::string > &bindVars ) {
  
    std::vector<const char *> bs;
    std::transform(bindVars.begin(), bindVars.end(), std::back_inserter(bs), std::string::c_str);

    return _cllExecSqlWithResult(icss, res, sql, bs);
}

/*
  Return a row from a previous cllExecSqlWithResult call.
*/
int
cllGetRow( icatSessionStruct *icss, int statementNumber ) {
    icatStmtStrct *myStatement = icss->stmtPtr[statementNumber];

    for ( int i = 0; i < myStatement->numOfCols; i++ ) {
        strcpy( ( char * )myStatement->resultValue[i], "" );
    }
    SQLRETURN stat =  SQLFetch( myStatement->stmtPtr );
    if ( stat != SQL_SUCCESS && stat != SQL_NO_DATA_FOUND ) {
        rodsLog( LOG_ERROR, "cllGetRow: SQLFetch failed: %d", stat );
        return -1;
    }
    if ( stat == SQL_NO_DATA_FOUND ) {
        _cllFreeStatementColumns( icss, statementNumber );
        myStatement->numOfCols = 0;
    }
    return 0;
}

/*
   Return the string needed to get the next value in a sequence item.
   The syntax varies between RDBMSes, so it is here, in the DBMS-specific code.
*/
int
cllNextValueString( const char *itemName, char *outString, int maxSize ) {
#ifdef ORA_ICAT
    snprintf( outString, maxSize, "%s.nextval", itemName );
#elif MY_ICAT
    snprintf( outString, maxSize, "%s_nextval()", itemName );
#else
    snprintf( outString, maxSize, "nextval('%s')", itemName );
#endif
    return 0;
}

int
cllCurrentValueString( const char *itemName, char *outString, int maxSize ) {
#ifdef ORA_ICAT
    snprintf( outString, maxSize, "%s.currval", itemName );
#elif MY_ICAT
    snprintf( outString, maxSize, "%s_currval()", itemName );
#else
    snprintf( outString, maxSize, "currval('%s')", itemName );
#endif
    return 0;
}

/*
   Free a statement (from a previous cllExecSqlWithResult call) and the
   corresponding resultValue array.
*/
int
cllFreeStatement( icatSessionStruct *icss, int statementNumber ) {

    icatStmtStrct * myStatement = icss->stmtPtr[statementNumber];
    if ( myStatement == NULL ) { /* already freed */
        return 0;
    }

    _cllFreeStatementColumns( icss, statementNumber );

    SQLRETURN stat = SQLFreeHandle( SQL_HANDLE_STMT, myStatement->stmtPtr );
    if ( stat != SQL_SUCCESS ) {
        rodsLog( LOG_ERROR, "cllFreeStatement SQLFreeHandle for statement error: %d", stat );
    }

    free( myStatement );

    icss->stmtPtr[statementNumber] = NULL; /* indicate that the statement is free */

    return 0;
}

/*
   Free the statement columns (from a previous cllExecSqlWithResult call),
   but not the whole statement.
*/
int
_cllFreeStatementColumns( icatSessionStruct *icss, int statementNumber ) {

    icatStmtStrct * myStatement = icss->stmtPtr[statementNumber];

    for ( int i = 0; i < myStatement->numOfCols; i++ ) {
        free( myStatement->resultValue[i] );
        myStatement->resultValue[i] = NULL;
        free( myStatement->resultColName[i] );
        myStatement->resultColName[i] = NULL;
    }
    return 0;
}
