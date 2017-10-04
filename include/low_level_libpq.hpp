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
#include "icatStructs.hpp"

#include <vector>
#include <string>
#include <functional>
#include <libpq-fe.h>

#define MAX_BIND_VARS 32000

class result_set {
public:
  result_set(std::function<int(int, int, PGresult *&)> _query, int _offset, int _maxrows);
  ~result_set();
  int next_row();
  bool has_row();
  int row_size();
  int size();
  void get_value(int _col, char *_buf, int _len);
  const char *get_value(int _col);
  void clear();
private:
  std::function<int(int, int, PGresult *&)> query_;
  PGresult *res_;
  int offset_;
  int maxrows_;
  int row_;
};

extern int cllBindVarCount;
extern const char *cllBindVars[MAX_BIND_VARS];
extern std::vector<result_set *> result_sets;

int execSql(const icatSessionStruct *icss, result_set **_resset, const std::string &sql, const std::vector<std::string> &bindVars = std::vector<std::string>());
int execSql( const icatSessionStruct *icss, const std::string &sql, const std::vector<std::string> &bindVars = std::vector<std::string>());
int execSql( const icatSessionStruct *icss, result_set **_resset, const std::function<std::string(int, int)> &_sqlgen, const std::vector<std::string> &bindVars = std::vector<std::string>(), int offset = 0, int maxrows = 256);
int cllConnect( icatSessionStruct *icss, const std::string &host, int port, const std::string &dbname );
int cllDisconnect( icatSessionStruct *icss );
int cllExecSqlNoResult( const icatSessionStruct *icss, const char *sql );
int cllExecSqlWithResult( const icatSessionStruct *icss, int *stmtNum, const char *sql );
int cllExecSqlWithResultBV( const icatSessionStruct *icss, int *stmtNum, const char *sql,
                            const std::vector<std::string> &bindVars );
int cllGetBindVars(std::vector<std::string> &bindVars);
int cllFreeStatement(int _resinx);

#endif	/* CLL_ODBC_HPP */
