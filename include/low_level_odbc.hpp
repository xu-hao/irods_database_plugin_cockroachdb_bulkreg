/*** Copyright (c), The Regents of the University of California            ***
 *** For more information please refer to files in the COPYRIGHT directory ***/
/*
  header file for the ODBC version of the icat low level routines,
  which is for Postgres or MySQL.
 */

#ifndef CLL_ODBC_HPP
#define CLL_ODBC_HPP

#include "sql.h"
#include "sqlext.h"

#include "rods.h"
#include "mid_level.hpp"

#include <vector>
#include <string>

#define MAX_BIND_VARS 32000

extern int cllBindVarCount;
extern const char *cllBindVars[MAX_BIND_VARS];

int cllConnect( icatSessionStruct *icss, const std::string &host, int port, const std::string &dbname );
int cllDisconnect( icatSessionStruct *icss );
int cllExecSqlNoResult( icatSessionStruct *icss, const char *sql );
int cllExecSqlWithResult( icatSessionStruct *icss, int *stmtNum, const char *sql );
int cllExecSqlWithResultBV( icatSessionStruct *icss, int *stmtNum, const char *sql,
                            std::vector<std::string> &bindVars );
int cllNextValue( icatSessionStruct *icss, rodsLong_t &ival );

#endif	/* CLL_ODBC_HPP */
