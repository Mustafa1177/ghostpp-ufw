/*

   Copyright [2008] [Trevor Hogan]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   CODE PORTED FROM THE ORIGINAL GHOST PROJECT: http://ghost.pwner.org/

*/

#ifdef GHOST_MYSQL

#include "ghost.h"
#include "util.h"
#include "config.h"
#include "ghostdb.h"
#include "ghostdbmysql.h"
#include "elorating2.h"

#include <signal.h>

#ifdef WIN32
 #include <winsock.h>
#endif

#include <mysql/mysql.h>
#include <boost/thread.hpp>

//
// CGHostDBMySQL
//

CGHostDBMySQL :: CGHostDBMySQL( CConfig *CFG ) : CGHostDB( CFG )
{
	m_Server = CFG->GetString( "db_mysql_server", string( ) );
	m_Database = CFG->GetString( "db_mysql_database", "ghost" );
	m_User = CFG->GetString( "db_mysql_user", string( ) );
	m_Password = CFG->GetString( "db_mysql_password", string( ) );
	m_Port = CFG->GetInt( "db_mysql_port", 0 );
	m_BotID = CFG->GetInt( "db_mysql_botid", 0 );
	m_NumConnections = 1;
	m_OutstandingCallables = 0;

	mysql_library_init( 0, NULL, NULL );

	// create the first connection

	CONSOLE_Print( "[MYSQL] connecting to database server" );
	MYSQL *Connection = NULL;

	if( !( Connection = mysql_init( NULL ) ) )
	{
		CONSOLE_Print( string( "[MYSQL] " ) + mysql_error( Connection ) );
		m_HasError = true;
		m_Error = "error initializing MySQL connection";
		return;
	}

	bool Reconnect = true;
	mysql_options( Connection, MYSQL_OPT_RECONNECT, &Reconnect );

	if( !( mysql_real_connect( Connection, m_Server.c_str( ), m_User.c_str( ), m_Password.c_str( ), m_Database.c_str( ), m_Port, NULL, 0 ) ) )
	{
		CONSOLE_Print( string( "[MYSQL] " ) + mysql_error( Connection ) );
		m_HasError = true;
		m_Error = "error connecting to MySQL server";
		return;
	}

	m_IdleConnections.push( Connection );
}

CGHostDBMySQL :: ~CGHostDBMySQL( )
{
	boost::mutex::scoped_lock lock(m_DatabaseMutex);
	CONSOLE_Print( "[MYSQL] closing " + UTIL_ToString( m_IdleConnections.size( ) ) + "/" + UTIL_ToString( m_NumConnections ) + " idle MySQL connections" );

	while( !m_IdleConnections.empty( ) )
	{
		mysql_close( (MYSQL *)m_IdleConnections.front( ) );
		m_IdleConnections.pop( );
	}

	if( m_OutstandingCallables > 0 )
		CONSOLE_Print( "[MYSQL] " + UTIL_ToString( m_OutstandingCallables ) + " outstanding callables were never recovered" );

	mysql_library_end( );
}

string CGHostDBMySQL :: GetStatus( )
{
	return "DB STATUS --- Connections: " + UTIL_ToString( m_IdleConnections.size( ) ) + "/" + UTIL_ToString( m_NumConnections ) + " idle. Outstanding callables: " + UTIL_ToString( m_OutstandingCallables ) + ".";
}

void CGHostDBMySQL :: RecoverCallable( CBaseCallable *callable )
{
	boost::mutex::scoped_lock lock(m_DatabaseMutex);
	CMySQLCallable *MySQLCallable = dynamic_cast<CMySQLCallable *>( callable );

	if( MySQLCallable )
	{
		if( m_IdleConnections.size( ) > 30 )
		{
			mysql_close( (MYSQL *)MySQLCallable->GetConnection( ) );
			--m_NumConnections;
		}
		else
			m_IdleConnections.push( MySQLCallable->GetConnection( ) );

		if( m_OutstandingCallables == 0 )
			CONSOLE_Print( "[MYSQL] recovered a mysql callable with zero outstanding" );
		else
			--m_OutstandingCallables;

		if( !MySQLCallable->GetError( ).empty( ) )
			CONSOLE_Print( "[MYSQL] error --- " + MySQLCallable->GetError( ) );
	}
	else
		CONSOLE_Print( "[MYSQL] tried to recover a non-mysql callable" );
}

void CGHostDBMySQL :: CreateThread( CBaseCallable *callable )
{
	try
	{
		boost :: thread Thread( boost :: ref( *callable ) );
	}
	catch( boost :: thread_resource_error tre )
	{
		CONSOLE_Print( "[MYSQL] error spawning thread on attempt #1 [" + string( tre.what( ) ) + "], pausing execution and trying again in 50ms" );
		MILLISLEEP( 50 );

		try
		{
			boost :: thread Thread( boost :: ref( *callable ) );
		}
		catch( boost :: thread_resource_error tre2 )
		{
			CONSOLE_Print( "[MYSQL] error spawning thread on attempt #2 [" + string( tre2.what( ) ) + "], giving up" );
			callable->SetReady( true );
		}
	}
}

CCallableAdminCount *CGHostDBMySQL :: ThreadedAdminCount( string server )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableAdminCount *Callable = new CMySQLCallableAdminCount( server, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableAdminCheck *CGHostDBMySQL :: ThreadedAdminCheck( string server, string user )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableAdminCheck *Callable = new CMySQLCallableAdminCheck( server, user, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableAdminAdd *CGHostDBMySQL :: ThreadedAdminAdd( string server, string user )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableAdminAdd *Callable = new CMySQLCallableAdminAdd( server, user, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableAdminRemove *CGHostDBMySQL :: ThreadedAdminRemove( string server, string user )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableAdminRemove *Callable = new CMySQLCallableAdminRemove( server, user, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableAdminList *CGHostDBMySQL :: ThreadedAdminList( string server )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableAdminList *Callable = new CMySQLCallableAdminList( server, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableBanCount *CGHostDBMySQL :: ThreadedBanCount( string server )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableBanCount *Callable = new CMySQLCallableBanCount( server, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableBanCheck *CGHostDBMySQL :: ThreadedBanCheck( string server, string user, string ip )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableBanCheck *Callable = new CMySQLCallableBanCheck( server, user, ip, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableBanAdd *CGHostDBMySQL :: ThreadedBanAdd( string server, string user, string ip, string gamename, string admin, string reason )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableBanAdd *Callable = new CMySQLCallableBanAdd( server, user, ip, gamename, admin, reason, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableBanRemove *CGHostDBMySQL :: ThreadedBanRemove( string server, string user )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableBanRemove *Callable = new CMySQLCallableBanRemove( server, user, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableBanRemove *CGHostDBMySQL :: ThreadedBanRemove( string user )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableBanRemove *Callable = new CMySQLCallableBanRemove( string( ), user, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableBanList *CGHostDBMySQL :: ThreadedBanList( string server )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableBanList *Callable = new CMySQLCallableBanList( server, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableGameAdd *CGHostDBMySQL :: ThreadedGameAdd( string server, string map, string gamename, string ownername, uint32_t duration, uint32_t gamestate, string creatorname, string creatorserver )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableGameAdd *Callable = new CMySQLCallableGameAdd( server, map, gamename, ownername, duration, gamestate, creatorname, creatorserver, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableGamePlayerAdd *CGHostDBMySQL :: ThreadedGamePlayerAdd( uint32_t gameid, string name, string ip, uint32_t spoofed, string spoofedrealm, uint32_t reserved, uint32_t loadingtime, uint32_t left, string leftreason, uint32_t team, uint32_t colour )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableGamePlayerAdd *Callable = new CMySQLCallableGamePlayerAdd( gameid, name, ip, spoofed, spoofedrealm, reserved, loadingtime, left, leftreason, team, colour, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableGamePlayerSummaryCheck *CGHostDBMySQL :: ThreadedGamePlayerSummaryCheck( string name )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableGamePlayerSummaryCheck *Callable = new CMySQLCallableGamePlayerSummaryCheck( name, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableDotAGameAdd *CGHostDBMySQL :: ThreadedDotAGameAdd( uint32_t gameid, uint32_t winner, uint32_t min, uint32_t sec )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableDotAGameAdd *Callable = new CMySQLCallableDotAGameAdd( gameid, winner, min, sec, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableDotAPlayerAdd *CGHostDBMySQL :: ThreadedDotAPlayerAdd( uint32_t gameid, uint32_t colour, uint32_t kills, uint32_t deaths, uint32_t creepkills, uint32_t creepdenies, uint32_t assists, uint32_t gold, uint32_t neutralkills, string item1, string item2, string item3, string item4, string item5, string item6, string hero, uint32_t newcolour, uint32_t towerkills, uint32_t raxkills, uint32_t courierkills )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableDotAPlayerAdd *Callable = new CMySQLCallableDotAPlayerAdd( gameid, colour, kills, deaths, creepkills, creepdenies, assists, gold, neutralkills, item1, item2, item3, item4, item5, item6, hero, newcolour, towerkills, raxkills, courierkills, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableDotAPlayerSummaryCheck *CGHostDBMySQL :: ThreadedDotAPlayerSummaryCheck( string name )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableDotAPlayerSummaryCheck *Callable = new CMySQLCallableDotAPlayerSummaryCheck( name, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableDownloadAdd *CGHostDBMySQL :: ThreadedDownloadAdd( string map, uint32_t mapsize, string name, string ip, uint32_t spoofed, string spoofedrealm, uint32_t downloadtime )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableDownloadAdd *Callable = new CMySQLCallableDownloadAdd( map, mapsize, name, ip, spoofed, spoofedrealm, downloadtime, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableScoreCheck *CGHostDBMySQL :: ThreadedScoreCheck( string category, string name, string server )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableScoreCheck *Callable = new CMySQLCallableScoreCheck( category, name, server, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableW3MMDPlayerAdd *CGHostDBMySQL :: ThreadedW3MMDPlayerAdd( string category, uint32_t gameid, uint32_t pid, string name, string flag, uint32_t leaver, uint32_t practicing )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableW3MMDPlayerAdd *Callable = new CMySQLCallableW3MMDPlayerAdd( category, gameid, pid, name, flag, leaver, practicing, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableW3MMDVarAdd *CGHostDBMySQL :: ThreadedW3MMDVarAdd( uint32_t gameid, map<VarP,int32_t> var_ints )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableW3MMDVarAdd *Callable = new CMySQLCallableW3MMDVarAdd( gameid, var_ints, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableW3MMDVarAdd *CGHostDBMySQL :: ThreadedW3MMDVarAdd( uint32_t gameid, map<VarP,double> var_reals )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableW3MMDVarAdd *Callable = new CMySQLCallableW3MMDVarAdd( gameid, var_reals, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

CCallableW3MMDVarAdd *CGHostDBMySQL :: ThreadedW3MMDVarAdd( uint32_t gameid, map<VarP,string> var_strings )
{
	void *Connection = GetIdleConnection( );

	if( !Connection )
		++m_NumConnections;

	CCallableW3MMDVarAdd *Callable = new CMySQLCallableW3MMDVarAdd( gameid, var_strings, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port );
	CreateThread( Callable );
	++m_OutstandingCallables;
	return Callable;
}

void *CGHostDBMySQL :: GetIdleConnection( )
{
	boost::mutex::scoped_lock lock(m_DatabaseMutex);
	void *Connection = NULL;

	if( !m_IdleConnections.empty( ) )
	{
		Connection = m_IdleConnections.front( );
		m_IdleConnections.pop( );
	}

	return Connection;
}

//
// unprototyped global helper functions
//

string MySQLEscapeString( void *conn, string str )
{
	char *to = new char[str.size( ) * 2 + 1];
	unsigned long size = mysql_real_escape_string( (MYSQL *)conn, to, str.c_str( ), str.size( ) );
	string result( to, size );
	delete [] to;
	return result;
}

vector<string> MySQLFetchRow( MYSQL_RES *res )
{
	vector<string> Result;

	MYSQL_ROW Row = mysql_fetch_row( res );

	if( Row )
	{
		unsigned long *Lengths;
		Lengths = mysql_fetch_lengths( res );

		for( unsigned int i = 0; i < mysql_num_fields( res ); ++i )
		{
			if( Row[i] )
				Result.push_back( string( Row[i], Lengths[i] ) );
			else
				Result.push_back( string( ) );
		}
	}

	return Result;
}

//
// global helper functions
//

uint32_t MySQLAdminCount( void *conn, string *error, uint32_t botid, string server )
{
	string EscServer = MySQLEscapeString( conn, server );
	uint32_t Count = 0;
	string Query = "SELECT COUNT(*) FROM admins WHERE server='" + EscServer + "'";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
	{
		MYSQL_RES *Result = mysql_store_result( (MYSQL *)conn );

		if( Result )
		{
			vector<string> Row = MySQLFetchRow( Result );

			if( Row.size( ) == 1 )
				Count = UTIL_ToUInt32( Row[0] );
			else
				*error = "error counting admins [" + server + "] - row doesn't have 1 column";

			mysql_free_result( Result );
		}
		else
			*error = mysql_error( (MYSQL *)conn );
	}

	return Count;
}

bool MySQLAdminCheck( void *conn, string *error, uint32_t botid, string server, string user )
{
	transform( user.begin( ), user.end( ), user.begin( ), (int(*)(int))tolower );
	string EscServer = MySQLEscapeString( conn, server );
	string EscUser = MySQLEscapeString( conn, user );
	bool IsAdmin = false;
	string Query = "SELECT * FROM admins WHERE server='" + EscServer + "' AND name='" + EscUser + "'";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
	{
		MYSQL_RES *Result = mysql_store_result( (MYSQL *)conn );

		if( Result )
		{
			vector<string> Row = MySQLFetchRow( Result );

			if( !Row.empty( ) )
				IsAdmin = true;

			mysql_free_result( Result );
		}
		else
			*error = mysql_error( (MYSQL *)conn );
	}

	return IsAdmin;
}

bool MySQLAdminAdd( void *conn, string *error, uint32_t botid, string server, string user )
{
	transform( user.begin( ), user.end( ), user.begin( ), (int(*)(int))tolower );
	string EscServer = MySQLEscapeString( conn, server );
	string EscUser = MySQLEscapeString( conn, user );
	bool Success = false;
	string Query = "INSERT INTO admins ( botid, server, name ) VALUES ( " + UTIL_ToString( botid ) + ", '" + EscServer + "', '" + EscUser + "' )";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		Success = true;

	return Success;
}

bool MySQLAdminRemove( void *conn, string *error, uint32_t botid, string server, string user )
{
	transform( user.begin( ), user.end( ), user.begin( ), (int(*)(int))tolower );
	string EscServer = MySQLEscapeString( conn, server );
	string EscUser = MySQLEscapeString( conn, user );
	bool Success = false;
	string Query = "DELETE FROM admins WHERE server='" + EscServer + "' AND name='" + EscUser + "'";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		Success = true;

	return Success;
}

vector<string> MySQLAdminList( void *conn, string *error, uint32_t botid, string server )
{
	string EscServer = MySQLEscapeString( conn, server );
	vector<string> AdminList;
	string Query = "SELECT name FROM admins WHERE server='" + EscServer + "'";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
	{
		MYSQL_RES *Result = mysql_store_result( (MYSQL *)conn );

		if( Result )
		{
			vector<string> Row = MySQLFetchRow( Result );

			while( !Row.empty( ) )
			{
				AdminList.push_back( Row[0] );
				Row = MySQLFetchRow( Result );
			}

			mysql_free_result( Result );
		}
		else
			*error = mysql_error( (MYSQL *)conn );
	}

	return AdminList;
}

uint32_t MySQLBanCount( void *conn, string *error, uint32_t botid, string server )
{
	string EscServer = MySQLEscapeString( conn, server );
	uint32_t Count = 0;
	string Query = "SELECT COUNT(*) FROM bans WHERE server='" + EscServer + "'";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
	{
		MYSQL_RES *Result = mysql_store_result( (MYSQL *)conn );

		if( Result )
		{
			vector<string> Row = MySQLFetchRow( Result );

			if( Row.size( ) == 1 )
				Count = UTIL_ToUInt32( Row[0] );
			else
				*error = "error counting bans [" + server + "] - row doesn't have 1 column";

			mysql_free_result( Result );
		}
		else
			*error = mysql_error( (MYSQL *)conn );
	}

	return Count;
}

CDBBan *MySQLBanCheck( void *conn, string *error, uint32_t botid, string server, string user, string ip )
{
	transform( user.begin( ), user.end( ), user.begin( ), (int(*)(int))tolower );
	string EscServer = MySQLEscapeString( conn, server );
	string EscUser = MySQLEscapeString( conn, user );
	string EscIP = MySQLEscapeString( conn, ip );
	CDBBan *Ban = NULL;
	string Query;

	if( ip.empty( ) )
		Query = "SELECT name, ip, DATE(date), gamename, `admin`, reason FROM bans WHERE server='" + EscServer + "' AND name='" + EscUser + "'";
	else
		Query = "SELECT name, ip, DATE(date), gamename, `admin`, reason FROM bans WHERE (server='" + EscServer + "' AND name='" + EscUser + "') OR ip='" + EscIP + "'";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
	{
		MYSQL_RES *Result = mysql_store_result( (MYSQL *)conn );

		if( Result )
		{
			vector<string> Row = MySQLFetchRow( Result );

			if( Row.size( ) == 6 )
				Ban = new CDBBan( server, Row[0], Row[1], Row[2], Row[3], Row[4], Row[5] );
			/* else
				*error = "error checking ban [" + server + " : " + user + "] - row doesn't have 6 columns"; */

			mysql_free_result( Result );
		}
		else
			*error = mysql_error( (MYSQL *)conn );
	}

	return Ban;
}

bool MySQLBanAdd( void *conn, string *error, uint32_t botid, string server, string user, string ip, string gamename, string admin, string reason )
{
	transform( user.begin( ), user.end( ), user.begin( ), (int(*)(int))tolower );
	string EscServer = MySQLEscapeString( conn, server );
	string EscUser = MySQLEscapeString( conn, user );
	string EscIP = MySQLEscapeString( conn, ip );
	string EscGameName = MySQLEscapeString( conn, gamename );
	string EscAdmin = MySQLEscapeString( conn, admin );
	string EscReason = MySQLEscapeString( conn, reason );
	bool Success = false;
	string Query = "INSERT INTO bans ( botid, server, name, ip, date, gamename, `admin`, reason ) VALUES ( " + UTIL_ToString( botid ) + ", '" + EscServer + "', '" + EscUser + "', '" + EscIP + "', CURDATE( ), '" + EscGameName + "', '" + EscAdmin + "', '" + EscReason + "' )";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		Success = true;

	return Success;
}

bool MySQLBanRemove( void *conn, string *error, uint32_t botid, string server, string user )
{
	transform( user.begin( ), user.end( ), user.begin( ), (int(*)(int))tolower );
	string EscServer = MySQLEscapeString( conn, server );
	string EscUser = MySQLEscapeString( conn, user );
	bool Success = false;
	string Query = "DELETE FROM bans WHERE server='" + EscServer + "' AND name='" + EscUser + "'";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		Success = true;

	return Success;
}

bool MySQLBanRemove( void *conn, string *error, uint32_t botid, string user )
{
	transform( user.begin( ), user.end( ), user.begin( ), (int(*)(int))tolower );
	string EscUser = MySQLEscapeString( conn, user );
	bool Success = false;
	string Query = "DELETE FROM bans WHERE name='" + EscUser + "'";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		Success = true;

	return Success;
}

vector<CDBBan *> MySQLBanList( void *conn, string *error, uint32_t botid, string server )
{
	string EscServer = MySQLEscapeString( conn, server );
	vector<CDBBan *> BanList;
	string Query = "SELECT name, ip, DATE(date), gamename, `admin`, reason FROM bans WHERE (server='" + EscServer + "' OR server='')";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
	{
		MYSQL_RES *Result = mysql_store_result( (MYSQL *)conn );

		if( Result )
		{
			vector<string> Row = MySQLFetchRow( Result );

			while( Row.size( ) == 6 )
			{
				BanList.push_back( new CDBBan( server, Row[0], Row[1], Row[2], Row[3], Row[4], Row[5] ) );
				Row = MySQLFetchRow( Result );
			}

			mysql_free_result( Result );
		}
		else
			*error = mysql_error( (MYSQL *)conn );
	}

	return BanList;
}

uint32_t MySQLGameAdd( void *conn, string *error, uint32_t botid, string server, string map, string gamename, string ownername, uint32_t duration, uint32_t gamestate, string creatorname, string creatorserver )
{
	uint32_t RowID = 0;
	string EscServer = MySQLEscapeString( conn, server );
	string EscMap = MySQLEscapeString( conn, map );
	string EscGameName = MySQLEscapeString( conn, gamename );
	string EscOwnerName = MySQLEscapeString( conn, ownername );
	string EscCreatorName = MySQLEscapeString( conn, creatorname );
	string EscCreatorServer = MySQLEscapeString( conn, creatorserver );
	string Query = "INSERT INTO games ( botid, server, map, datetime, gamename, ownername, duration, gamestate, creatorname, creatorserver ) VALUES ( " + UTIL_ToString( botid ) + ", '" + EscServer + "', '" + EscMap + "', NOW( ), '" + EscGameName + "', '" + EscOwnerName + "', " + UTIL_ToString( duration ) + ", " + UTIL_ToString( gamestate ) + ", '" + EscCreatorName + "', '" + EscCreatorServer + "' )";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		RowID = mysql_insert_id( (MYSQL *)conn );

	return RowID;
}

uint32_t MySQLGamePlayerAdd( void *conn, string *error, uint32_t botid, uint32_t gameid, string name, string ip, uint32_t spoofed, string spoofedrealm, uint32_t reserved, uint32_t loadingtime, uint32_t left, string leftreason, uint32_t team, uint32_t colour )
{
	transform( name.begin( ), name.end( ), name.begin( ), (int(*)(int))tolower );
	uint32_t RowID = 0;
	string EscName = MySQLEscapeString( conn, name );
	string EscIP = MySQLEscapeString( conn, ip );
	string EscSpoofedRealm = MySQLEscapeString( conn, spoofedrealm );
	string EscLeftReason = MySQLEscapeString( conn, leftreason );
	string Query = "INSERT INTO gameplayers ( botid, gameid, name, ip, spoofed, reserved, loadingtime, `left`, leftreason, team, colour, spoofedrealm ) VALUES ( " + UTIL_ToString( botid ) + ", " + UTIL_ToString( gameid ) + ", '" + EscName + "', '" + EscIP + "', " + UTIL_ToString( spoofed ) + ", " + UTIL_ToString( reserved ) + ", " + UTIL_ToString( loadingtime ) + ", " + UTIL_ToString( left ) + ", '" + EscLeftReason + "', " + UTIL_ToString( team ) + ", " + UTIL_ToString( colour ) + ", '" + EscSpoofedRealm + "' )";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		RowID = mysql_insert_id( (MYSQL *)conn );

	return RowID;
}

CDBGamePlayerSummary *MySQLGamePlayerSummaryCheck( void *conn, string *error, uint32_t botid, string name )
{
	transform( name.begin( ), name.end( ), name.begin( ), (int(*)(int))tolower );
	string EscName = MySQLEscapeString( conn, name );
	CDBGamePlayerSummary *GamePlayerSummary = NULL;
	string Query = "SELECT MIN(DATE(datetime)), MAX(DATE(datetime)), COUNT(*), MIN(loadingtime), AVG(loadingtime), MAX(loadingtime), MIN(`left`/duration)*100, AVG(`left`/duration)*100, MAX(`left`/duration)*100, MIN(duration), AVG(duration), MAX(duration) FROM gameplayers LEFT JOIN games ON games.id=gameid WHERE name LIKE '" + EscName + "'";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
	{
		MYSQL_RES *Result = mysql_store_result( (MYSQL *)conn );

		if( Result )
		{
			vector<string> Row = MySQLFetchRow( Result );

			if( Row.size( ) == 12 )
			{
				string FirstGameDateTime = Row[0];
				string LastGameDateTime = Row[1];
				uint32_t TotalGames = UTIL_ToUInt32( Row[2] );
				uint32_t MinLoadingTime = UTIL_ToUInt32( Row[3] );
				uint32_t AvgLoadingTime = UTIL_ToUInt32( Row[4] );
				uint32_t MaxLoadingTime = UTIL_ToUInt32( Row[5] );
				uint32_t MinLeftPercent = UTIL_ToUInt32( Row[6] );
				uint32_t AvgLeftPercent = UTIL_ToUInt32( Row[7] );
				uint32_t MaxLeftPercent = UTIL_ToUInt32( Row[8] );
				uint32_t MinDuration = UTIL_ToUInt32( Row[9] );
				uint32_t AvgDuration = UTIL_ToUInt32( Row[10] );
				uint32_t MaxDuration = UTIL_ToUInt32( Row[11] );
				GamePlayerSummary = new CDBGamePlayerSummary( string( ), name, FirstGameDateTime, LastGameDateTime, TotalGames, MinLoadingTime, AvgLoadingTime, MaxLoadingTime, MinLeftPercent, AvgLeftPercent, MaxLeftPercent, MinDuration, AvgDuration, MaxDuration );
			}
			else
				*error = "error checking gameplayersummary [" + name + "] - row doesn't have 12 columns";

			mysql_free_result( Result );
		}
		else
			*error = mysql_error( (MYSQL *)conn );
	}

	return GamePlayerSummary;
}

uint32_t MySQLDotAGameAdd( void *conn, string *error, uint32_t botid, uint32_t gameid, uint32_t winner, uint32_t min, uint32_t sec )
{
	uint32_t RowID = 0;
	string Query = "INSERT INTO dotagames ( botid, gameid, winner, min, sec ) VALUES ( " + UTIL_ToString( botid ) + ", " + UTIL_ToString( gameid ) + ", " + UTIL_ToString( winner ) + ", " + UTIL_ToString( min ) + ", " + UTIL_ToString( sec ) + " )";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		RowID = mysql_insert_id( (MYSQL *)conn );

	return RowID;
}

uint32_t MySQLDotAPlayerAdd( void *conn, string *error, uint32_t botid, uint32_t gameid, uint32_t colour, uint32_t kills, uint32_t deaths, uint32_t creepkills, uint32_t creepdenies, uint32_t assists, uint32_t gold, uint32_t neutralkills, string item1, string item2, string item3, string item4, string item5, string item6, string hero, uint32_t newcolour, uint32_t towerkills, uint32_t raxkills, uint32_t courierkills )
{
	uint32_t RowID = 0;
	string EscItem1 = MySQLEscapeString( conn, item1 );
	string EscItem2 = MySQLEscapeString( conn, item2 );
	string EscItem3 = MySQLEscapeString( conn, item3 );
	string EscItem4 = MySQLEscapeString( conn, item4 );
	string EscItem5 = MySQLEscapeString( conn, item5 );
	string EscItem6 = MySQLEscapeString( conn, item6 );
	string EscHero = MySQLEscapeString( conn, hero );
	string Query = "INSERT INTO dotaplayers ( botid, gameid, colour, kills, deaths, creepkills, creepdenies, assists, gold, neutralkills, item1, item2, item3, item4, item5, item6, hero, newcolour, towerkills, raxkills, courierkills ) VALUES ( " + UTIL_ToString( botid ) + ", " + UTIL_ToString( gameid ) + ", " + UTIL_ToString( colour ) + ", " + UTIL_ToString( kills ) + ", " + UTIL_ToString( deaths ) + ", " + UTIL_ToString( creepkills ) + ", " + UTIL_ToString( creepdenies ) + ", " + UTIL_ToString( assists ) + ", " + UTIL_ToString( gold ) + ", " + UTIL_ToString( neutralkills ) + ", '" + EscItem1 + "', '" + EscItem2 + "', '" + EscItem3 + "', '" + EscItem4 + "', '" + EscItem5 + "', '" + EscItem6 + "', '" + EscHero + "', " + UTIL_ToString( newcolour ) + ", " + UTIL_ToString( towerkills ) + ", " + UTIL_ToString( raxkills ) + ", " + UTIL_ToString( courierkills ) + " )";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		RowID = mysql_insert_id( (MYSQL *)conn );

	return RowID;
}

CDBDotAPlayerSummary *MySQLDotAPlayerSummaryCheck( void *conn, string *error, uint32_t botid, string name )
{
	transform( name.begin( ), name.end( ), name.begin( ), (int(*)(int))tolower );
	string EscName = MySQLEscapeString( conn, name );
	CDBDotAPlayerSummary *DotAPlayerSummary = NULL;
	string Query = "SELECT COUNT(dotaplayers.id), SUM(kills), SUM(deaths), SUM(creepkills), SUM(creepdenies), SUM(assists), SUM(neutralkills), SUM(towerkills), SUM(raxkills), SUM(courierkills) FROM gameplayers LEFT JOIN games ON games.id=gameplayers.gameid LEFT JOIN dotaplayers ON dotaplayers.gameid=games.id AND dotaplayers.colour=gameplayers.colour WHERE name LIKE '" + EscName + "'";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
	{
		MYSQL_RES *Result = mysql_store_result( (MYSQL *)conn );

		if( Result )
		{
			vector<string> Row = MySQLFetchRow( Result );

			if( Row.size( ) == 10 )
			{
				uint32_t TotalGames = UTIL_ToUInt32( Row[0] );

				if( TotalGames > 0 )
				{
					uint32_t TotalWins = 0;
					uint32_t TotalLosses = 0;
					uint32_t TotalKills = UTIL_ToUInt32( Row[1] );
					uint32_t TotalDeaths = UTIL_ToUInt32( Row[2] );
					uint32_t TotalCreepKills = UTIL_ToUInt32( Row[3] );
					uint32_t TotalCreepDenies = UTIL_ToUInt32( Row[4] );
					uint32_t TotalAssists = UTIL_ToUInt32( Row[5] );
					uint32_t TotalNeutralKills = UTIL_ToUInt32( Row[6] );
					uint32_t TotalTowerKills = UTIL_ToUInt32( Row[7] );
					uint32_t TotalRaxKills = UTIL_ToUInt32( Row[8] );
					uint32_t TotalCourierKills = UTIL_ToUInt32( Row[9] );

					// calculate total wins

					string Query2 = "SELECT COUNT(*) FROM gameplayers LEFT JOIN games ON games.id=gameplayers.gameid LEFT JOIN dotaplayers ON dotaplayers.gameid=games.id AND dotaplayers.colour=gameplayers.colour LEFT JOIN dotagames ON games.id=dotagames.gameid WHERE name='" + EscName + "' AND ((winner=1 AND dotaplayers.newcolour>=1 AND dotaplayers.newcolour<=5) OR (winner=2 AND dotaplayers.newcolour>=7 AND dotaplayers.newcolour<=11))";

					if( mysql_real_query( (MYSQL *)conn, Query2.c_str( ), Query2.size( ) ) != 0 )
						*error = mysql_error( (MYSQL *)conn );
					else
					{
						MYSQL_RES *Result2 = mysql_store_result( (MYSQL *)conn );

						if( Result2 )
						{
							vector<string> Row2 = MySQLFetchRow( Result2 );

							if( Row2.size( ) == 1 )
								TotalWins = UTIL_ToUInt32( Row2[0] );
							else
								*error = "error checking dotaplayersummary wins [" + name + "] - row doesn't have 1 column";

							mysql_free_result( Result2 );
						}
						else
							*error = mysql_error( (MYSQL *)conn );
					}

					// calculate total losses

					string Query3 = "SELECT COUNT(*) FROM gameplayers LEFT JOIN games ON games.id=gameplayers.gameid LEFT JOIN dotaplayers ON dotaplayers.gameid=games.id AND dotaplayers.colour=gameplayers.colour LEFT JOIN dotagames ON games.id=dotagames.gameid WHERE name='" + EscName + "' AND ((winner=2 AND dotaplayers.newcolour>=1 AND dotaplayers.newcolour<=5) OR (winner=1 AND dotaplayers.newcolour>=7 AND dotaplayers.newcolour<=11))";

					if( mysql_real_query( (MYSQL *)conn, Query3.c_str( ), Query3.size( ) ) != 0 )
						*error = mysql_error( (MYSQL *)conn );
					else
					{
						MYSQL_RES *Result3 = mysql_store_result( (MYSQL *)conn );

						if( Result3 )
						{
							vector<string> Row3 = MySQLFetchRow( Result3 );

							if( Row3.size( ) == 1 )
								TotalLosses = UTIL_ToUInt32( Row3[0] );
							else
								*error = "error checking dotaplayersummary losses [" + name + "] - row doesn't have 1 column";

							mysql_free_result( Result3 );
						}
						else
							*error = mysql_error( (MYSQL *)conn );
					}

					// done

					DotAPlayerSummary = new CDBDotAPlayerSummary( string( ), name, TotalGames, TotalWins, TotalLosses, TotalKills, TotalDeaths, TotalCreepKills, TotalCreepDenies, TotalAssists, TotalNeutralKills, TotalTowerKills, TotalRaxKills, TotalCourierKills );
				}
			}
			else
				*error = "error checking dotaplayersummary [" + name + "] - row doesn't have 10 columns";

			mysql_free_result( Result );
		}
		else
			*error = mysql_error( (MYSQL *)conn );
	}

	return DotAPlayerSummary;
}

bool MySQLDownloadAdd( void *conn, string *error, uint32_t botid, string map, uint32_t mapsize, string name, string ip, uint32_t spoofed, string spoofedrealm, uint32_t downloadtime )
{
	bool Success = false;
	string EscMap = MySQLEscapeString( conn, map );
	string EscName = MySQLEscapeString( conn, name );
	string EscIP = MySQLEscapeString( conn, ip );
	string EscSpoofedRealm = MySQLEscapeString( conn, spoofedrealm );
	string Query = "INSERT INTO downloads ( botid, map, mapsize, datetime, name, ip, spoofed, spoofedrealm, downloadtime ) VALUES ( " + UTIL_ToString( botid ) + ", '" + EscMap + "', " + UTIL_ToString( mapsize ) + ", NOW( ), '" + EscName + "', '" + EscIP + "', " + UTIL_ToString( spoofed ) + ", '" + EscSpoofedRealm + "', " + UTIL_ToString( downloadtime ) + " )";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		Success = true;

	return Success;
}

double MySQLScoreCheck( void *conn, string *error, uint32_t botid, string category, string name, string server )
{
	transform( name.begin( ), name.end( ), name.begin( ), (int(*)(int))tolower );
	string EscCategory = MySQLEscapeString( conn, category );
	string EscName = MySQLEscapeString( conn, name );
	string EscServer = MySQLEscapeString( conn, server );
	double Score = -100000.0;
	string Query = "SELECT score FROM scores WHERE category='" + EscCategory + "' AND name='" + EscName + "' AND server='" + EscServer + "'";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
	{
		MYSQL_RES *Result = mysql_store_result( (MYSQL *)conn );

		if( Result )
		{
			vector<string> Row = MySQLFetchRow( Result );

			if( Row.size( ) == 1 )
				Score = UTIL_ToDouble( Row[0] );
			/* else
				*error = "error checking score [" + category + " : " + name + " : " + server + "] - row doesn't have 1 column"; */

			mysql_free_result( Result );
		}
		else
			*error = mysql_error( (MYSQL *)conn );
	}

	return Score;
}

uint32_t MySQLW3MMDPlayerAdd( void *conn, string *error, uint32_t botid, string category, uint32_t gameid, uint32_t pid, string name, string flag, uint32_t leaver, uint32_t practicing )
{
	transform( name.begin( ), name.end( ), name.begin( ), (int(*)(int))tolower );
	uint32_t RowID = 0;
	string EscCategory = MySQLEscapeString( conn, category );
	string EscName = MySQLEscapeString( conn, name );
	string EscFlag = MySQLEscapeString( conn, flag );
	string Query = "INSERT INTO w3mmdplayers ( botid, category, gameid, pid, name, flag, leaver, practicing ) VALUES ( " + UTIL_ToString( botid ) + ", '" + EscCategory + "', " + UTIL_ToString( gameid ) + ", " + UTIL_ToString( pid ) + ", '" + EscName + "', '" + EscFlag + "', " + UTIL_ToString( leaver ) + ", " + UTIL_ToString( practicing ) + " )";

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		RowID = mysql_insert_id( (MYSQL *)conn );

	return RowID;
}

bool MySQLW3MMDVarAdd( void *conn, string *error, uint32_t botid, uint32_t gameid, map<VarP,int32_t> var_ints )
{
	if( var_ints.empty( ) )
		return false;

	bool Success = false;
	string Query;

	for( map<VarP,int32_t> :: iterator i = var_ints.begin( ); i != var_ints.end( ); ++i )
	{
		string EscVarName = MySQLEscapeString( conn, i->first.second );

		if( Query.empty( ) )
			Query = "INSERT INTO w3mmdvars ( botid, gameid, pid, varname, value_int ) VALUES ( " + UTIL_ToString( botid ) + ", " + UTIL_ToString( gameid ) + ", " + UTIL_ToString( i->first.first ) + ", '" + EscVarName + "', " + UTIL_ToString( i->second ) + " )";
		else
			Query += ", ( " + UTIL_ToString( botid ) + ", " + UTIL_ToString( gameid ) + ", " + UTIL_ToString( i->first.first ) + ", '" + EscVarName + "', " + UTIL_ToString( i->second ) + " )";
	}

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		Success = true;

	return Success;
}

bool MySQLW3MMDVarAdd( void *conn, string *error, uint32_t botid, uint32_t gameid, map<VarP,double> var_reals )
{
	if( var_reals.empty( ) )
		return false;

	bool Success = false;
	string Query;

	for( map<VarP,double> :: iterator i = var_reals.begin( ); i != var_reals.end( ); ++i )
	{
		string EscVarName = MySQLEscapeString( conn, i->first.second );

		if( Query.empty( ) )
			Query = "INSERT INTO w3mmdvars ( botid, gameid, pid, varname, value_real ) VALUES ( " + UTIL_ToString( botid ) + ", " + UTIL_ToString( gameid ) + ", " + UTIL_ToString( i->first.first ) + ", '" + EscVarName + "', " + UTIL_ToString( i->second, 10 ) + " )";
		else
			Query += ", ( " + UTIL_ToString( botid ) + ", " + UTIL_ToString( gameid ) + ", " + UTIL_ToString( i->first.first ) + ", '" + EscVarName + "', " + UTIL_ToString( i->second, 10 ) + " )";
	}

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		Success = true;

	return Success;
}

bool MySQLW3MMDVarAdd( void *conn, string *error, uint32_t botid, uint32_t gameid, map<VarP,string> var_strings )
{
	if( var_strings.empty( ) )
		return false;

	bool Success = false;
	string Query;

	for( map<VarP,string> :: iterator i = var_strings.begin( ); i != var_strings.end( ); ++i )
	{
		string EscVarName = MySQLEscapeString( conn, i->first.second );
		string EscValueString = MySQLEscapeString( conn, i->second );

		if( Query.empty( ) )
			Query = "INSERT INTO w3mmdvars ( botid, gameid, pid, varname, value_string ) VALUES ( " + UTIL_ToString( botid ) + ", " + UTIL_ToString( gameid ) + ", " + UTIL_ToString( i->first.first ) + ", '" + EscVarName + "', '" + EscValueString + "' )";
		else
			Query += ", ( " + UTIL_ToString( botid ) + ", " + UTIL_ToString( gameid ) + ", " + UTIL_ToString( i->first.first ) + ", '" + EscVarName + "', '" + EscValueString + "' )";
	}

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		Success = true;

	return Success;
}

//
// MySQL Callables
//

void CMySQLCallable :: Init( )
{
	CBaseCallable :: Init( );

#ifndef WIN32
	// disable SIGPIPE since this is (or should be) a new thread and it doesn't inherit the spawning thread's signal handlers
	// MySQL should automatically disable SIGPIPE when we initialize it but we do so anyway here

	signal( SIGPIPE, SIG_IGN );
#endif

	mysql_thread_init( );

	if( !m_Connection )
	{
		if( !( m_Connection = mysql_init( NULL ) ) )
			m_Error = mysql_error( (MYSQL *)m_Connection );

		bool Reconnect = true;
		mysql_options( (MYSQL *)m_Connection, MYSQL_OPT_RECONNECT, &Reconnect );

		if( !( mysql_real_connect( (MYSQL *)m_Connection, m_SQLServer.c_str( ), m_SQLUser.c_str( ), m_SQLPassword.c_str( ), m_SQLDatabase.c_str( ), m_SQLPort, NULL, 0 ) ) )
			m_Error = mysql_error( (MYSQL *)m_Connection );
	}
	else if( mysql_ping( (MYSQL *)m_Connection ) != 0 )
		m_Error = mysql_error( (MYSQL *)m_Connection );
}

void CMySQLCallable :: Close( )
{
	mysql_thread_end( );

	CBaseCallable :: Close( );
}

void CMySQLCallableAdminCount :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLAdminCount( m_Connection, &m_Error, m_SQLBotID, m_Server );

	Close( );
}

void CMySQLCallableAdminCheck :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLAdminCheck( m_Connection, &m_Error, m_SQLBotID, m_Server, m_User );

	Close( );
}

void CMySQLCallableAdminAdd :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLAdminAdd( m_Connection, &m_Error, m_SQLBotID, m_Server, m_User );

	Close( );
}

void CMySQLCallableAdminRemove :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLAdminRemove( m_Connection, &m_Error, m_SQLBotID, m_Server, m_User );

	Close( );
}

void CMySQLCallableAdminList :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLAdminList( m_Connection, &m_Error, m_SQLBotID, m_Server );

	Close( );
}

void CMySQLCallableBanCount :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLBanCount( m_Connection, &m_Error, m_SQLBotID, m_Server );

	Close( );
}

void CMySQLCallableBanCheck :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLBanCheck( m_Connection, &m_Error, m_SQLBotID, m_Server, m_User, m_IP );

	Close( );
}

void CMySQLCallableBanAdd :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLBanAdd( m_Connection, &m_Error, m_SQLBotID, m_Server, m_User, m_IP, m_GameName, m_Admin, m_Reason );

	Close( );
}

void CMySQLCallableBanRemove :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
	{
		if( m_Server.empty( ) )
			m_Result = MySQLBanRemove( m_Connection, &m_Error, m_SQLBotID, m_User );
		else
			m_Result = MySQLBanRemove( m_Connection, &m_Error, m_SQLBotID, m_Server, m_User );
	}

	Close( );
}

void CMySQLCallableBanList :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLBanList( m_Connection, &m_Error, m_SQLBotID, m_Server );

	Close( );
}

void CMySQLCallableGameAdd :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLGameAdd( m_Connection, &m_Error, m_SQLBotID, m_Server, m_Map, m_GameName, m_OwnerName, m_Duration, m_GameState, m_CreatorName, m_CreatorServer );

	Close( );
}

void CMySQLCallableGamePlayerAdd :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLGamePlayerAdd( m_Connection, &m_Error, m_SQLBotID, m_GameID, m_Name, m_IP, m_Spoofed, m_SpoofedRealm, m_Reserved, m_LoadingTime, m_Left, m_LeftReason, m_Team, m_Colour );

	Close( );
}

void CMySQLCallableGamePlayerSummaryCheck :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLGamePlayerSummaryCheck( m_Connection, &m_Error, m_SQLBotID, m_Name );

	Close( );
}

void CMySQLCallableDotAGameAdd :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLDotAGameAdd( m_Connection, &m_Error, m_SQLBotID, m_GameID, m_Winner, m_Min, m_Sec );

	Close( );
}

void CMySQLCallableDotAPlayerAdd :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLDotAPlayerAdd( m_Connection, &m_Error, m_SQLBotID, m_GameID, m_Colour, m_Kills, m_Deaths, m_CreepKills, m_CreepDenies, m_Assists, m_Gold, m_NeutralKills, m_Item1, m_Item2, m_Item3, m_Item4, m_Item5, m_Item6, m_Hero, m_NewColour, m_TowerKills, m_RaxKills, m_CourierKills );

	Close( );
}

void CMySQLCallableDotAPlayerSummaryCheck :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLDotAPlayerSummaryCheck( m_Connection, &m_Error, m_SQLBotID, m_Name );

	Close( );
}

void CMySQLCallableDownloadAdd :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLDownloadAdd( m_Connection, &m_Error, m_SQLBotID, m_Map, m_MapSize, m_Name, m_IP, m_Spoofed, m_SpoofedRealm, m_DownloadTime );

	Close( );
}

void CMySQLCallableScoreCheck :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLScoreCheck( m_Connection, &m_Error, m_SQLBotID, m_Category, m_Name, m_Server );

	Close( );
}

void CMySQLCallableW3MMDPlayerAdd :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
		m_Result = MySQLW3MMDPlayerAdd( m_Connection, &m_Error, m_SQLBotID, m_Category, m_GameID, m_PID, m_Name, m_Flag, m_Leaver, m_Practicing );

	Close( );
}

void CMySQLCallableW3MMDVarAdd :: operator( )( )
{
	Init( );

	if( m_Error.empty( ) )
	{
		if( m_ValueType == VALUETYPE_INT )
			m_Result = MySQLW3MMDVarAdd( m_Connection, &m_Error, m_SQLBotID, m_GameID, m_VarInts );
		else if( m_ValueType == VALUETYPE_REAL )
			m_Result = MySQLW3MMDVarAdd( m_Connection, &m_Error, m_SQLBotID, m_GameID, m_VarReals );
		else
			m_Result = MySQLW3MMDVarAdd( m_Connection, &m_Error, m_SQLBotID, m_GameID, m_VarStrings );
	}

	Close( );
}


//==============================================================================================================================//
//														  New stuff																//
//==============================================================================================================================//

//correct, bring ir back (2)!!!!!!!!
/*
uint32_t MySQLDotAPlayerAddNew( void *conn, string *error, string name, string servername, uint32_t rating, uint32_t ratingpeak, uint32_t games, uint32_t wins, uint32_t loses, uint32_t kills, uint32_t deaths, uint32_t assists, uint32_t creepkills, uint32_t creepdenies, uint32_t neutralkills, uint32_t towerkills, uint32_t raxkills, uint32_t courierkills, uint32_t leaves, uint32_t playtime )
{
	uint32_t RowID = 0;
	string EscName = MySQLEscapeString( conn, name );
	string EscServer = MySQLEscapeString( conn, servername );

	string Query = "INSERT INTO `dotaplayerstats` ( name, servername, rating, ratingpeak, games, wins, loses, kills, deaths, assists, creepkills, creepdenies, neutralkills, towerkills, raxkills, courierkills, leaves, playtime) VALUES ( " + EscName + ", " + EscServer + ", " + UTIL_ToString( rating ) + ", " + UTIL_ToString( ratingpeak ) + ", " + UTIL_ToString( games ) + ", " + UTIL_ToString( wins ) + ", " + UTIL_ToString( loses ) + ", " + UTIL_ToString( kills ) + ", " + UTIL_ToString( deaths ) + ", " + UTIL_ToString( assists ) + ", '" + UTIL_ToString( creepkills ) + "', '" + UTIL_ToString( creepdenies ) + "', '" + UTIL_ToString( neutralkills ) + "', '" + UTIL_ToString( towerkills ) + "', '" + UTIL_ToString( raxkills ) + "', '" + UTIL_ToString( courierkills ) + "', '" + UTIL_ToString( leaves ) + "', " + UTIL_ToString( playtime ) + " )";
																																																									// "INSERT INTO `dotaplayerstats` (`id`, `name`, `server`, `rating`, `ratingpeak`, `games`, `wins`, `loses`, `kills`, `deaths`, `assists`, `creepkills`, `creepdenies`, `neutralkills`, `towerkills`, `raxkills`, `courierkills`, `leaves`, `playtime`, `joindate`, `unused01`) VALUES (NULL, '', '', '1500', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', current_timestamp(), '') "

	if( mysql_real_query( (MYSQL *)conn, Query.c_str( ), Query.size( ) ) != 0 )
		*error = mysql_error( (MYSQL *)conn );
	else
		RowID = (uint32_t)mysql_insert_id( (MYSQL *)conn );

	return RowID;
}
*/

uint32_t MySQLDotAPlayerAddNew(void* conn, string* error, uint32_t botid, uint32_t gameid, uint32_t colour, uint32_t kills, uint32_t deaths, uint32_t creepkills, uint32_t creepdenies, uint32_t assists, uint32_t gold, uint32_t neutralkills, string item1, string item2, string item3, string item4, string item5, string item6, string hero, uint32_t newcolour, uint32_t towerkills, uint32_t raxkills, uint32_t courierkills)
{
	uint32_t RowID = 0;
	string EscItem1 = MySQLEscapeString(conn, item1);
	string EscItem2 = MySQLEscapeString(conn, item2);
	string EscItem3 = MySQLEscapeString(conn, item3);
	string EscItem4 = MySQLEscapeString(conn, item4);
	string EscItem5 = MySQLEscapeString(conn, item5);
	string EscItem6 = MySQLEscapeString(conn, item6);
	string EscHero = MySQLEscapeString(conn, hero);
	string Query = "INSERT INTO dotaplayers ( botid, gameid, colour, kills, deaths, creepkills, creepdenies, assists, gold, neutralkills, item1, item2, item3, item4, item5, item6, hero, newcolour, towerkills, raxkills, courierkills ) VALUES ( " + UTIL_ToString(botid) + ", " + UTIL_ToString(gameid) + ", " + UTIL_ToString(colour) + ", " + UTIL_ToString(kills) + ", " + UTIL_ToString(deaths) + ", " + UTIL_ToString(creepkills) + ", " + UTIL_ToString(creepdenies) + ", " + UTIL_ToString(assists) + ", " + UTIL_ToString(gold) + ", " + UTIL_ToString(neutralkills) + ", '" + EscItem1 + "', '" + EscItem2 + "', '" + EscItem3 + "', '" + EscItem4 + "', '" + EscItem5 + "', '" + EscItem6 + "', '" + EscHero + "', " + UTIL_ToString(newcolour) + ", " + UTIL_ToString(towerkills) + ", " + UTIL_ToString(raxkills) + ", " + UTIL_ToString(courierkills) + " )";

	if (mysql_real_query((MYSQL*)conn, Query.c_str(), Query.size()) != 0)
		*error = mysql_error((MYSQL*)conn);
	else
		RowID = (uint32_t)mysql_insert_id((MYSQL*)conn);

	return RowID;
}

//CDBDotAPlayerSummary *MySQLDotAPlayerSummaryCheckNew( void *conn, string *error, uint32_t botid, string name, string formula, string mingames, string gamestate )
CDBDotAPlayerSummaryNew* MySQLDotAPlayerSummaryCheckNew(void* conn, string* error, string name, string servername, string formula, string mingames)
{
	transform(name.begin(), name.end(), name.begin(), (int(*)(int))tolower);
	string EscName = MySQLEscapeString(conn, name);
	string EscServerName = MySQLEscapeString(conn, servername);
	uint32_t Rank = 0;
	double Score = -10000;
	string server = string();
	CDBDotAPlayerSummaryNew* DotAPlayerSummary = NULL;
	//	string Query = "select totgames,wins,losses,killstotal,deathstotal,creepkillstotal,creepdeniestotal,assiststotal,neutralkillstotal,towerkillstotal,raxkillstotal,courierkillstotal,kills,deaths,creepkills,creepdenies,assists,neutralkills,towerkills,raxkills,courierkills, ("+formula+") as totalscore,server from(select *, (kills/deaths) as killdeathratio, (totgames-wins) as losses from (select gp.name as name,ga.server as server,gp.gameid as gameid, gp.colour as colour, avg(dp.courierkills) as courierkills, sum(dp.raxkills) as raxkillstotal, sum(dp.towerkills) as towerkillstotal, sum(dp.assists) as assiststotal,sum(dp.courierkills) as courierkillstotal, sum(dp.creepdenies) as creepdeniestotal, sum(dp.creepkills) as creepkillstotal,sum(dp.neutralkills) as neutralkillstotal, sum(dp.deaths) as deathstotal, sum(dp.kills) as killstotal,avg(dp.raxkills) as raxkills,avg(dp.towerkills) as towerkills, avg(dp.assists) as assists, avg(dp.creepdenies) as creepdenies, avg(dp.creepkills) as creepkills,avg(dp.neutralkills) as neutralkills, avg(dp.deaths) as deaths, avg(dp.kills) as kills,count(*) as totgames, SUM(case when((dg.winner = 1 and dp.newcolour < 6) or (dg.winner = 2 and dp.newcolour > 6)) then 1 else 0 end) as wins from gameplayers as gp, dotagames as dg, games as ga,dotaplayers as dp where dg.winner <> 0 and dp.gameid = gp.gameid and dg.gameid = dp.gameid and dp.gameid = ga.id and gp.gameid = dg.gameid and gp.colour = dp.colour and gp.name='"+name+"' group by gp.name) as h) as i";
	string Query = "SELECT name, servername, rating, ratingpeak, games, wins, loses, kills, deaths, assists, creepkills, creepdenies, neutralkills, towerkills, raxkills, courierkills, leaves, playtime, joindate FROM `dotaplayerstats` WHERE `name` = '" + EscName + "' AND `servername` = '" + EscServerName + "' LIMIT 1";
	//				0			1		2		3			4		5	6		7		8		9			10			11			12			13			14			15			16		17			18
	if (mysql_real_query((MYSQL*)conn, Query.c_str(), Query.size()) != 0)
		*error = mysql_error((MYSQL*)conn);
	else
	{
		MYSQL_RES* Result = mysql_store_result((MYSQL*)conn);

		if (Result)
		{
			vector<string> Row = MySQLFetchRow(Result);

			if (Row.size() == 19)
			{
				string ResPlayerName = Row[0];
				string ResServerName = Row[1];
				uint32_t TotalGames = UTIL_ToUInt32(Row[4]);

				if (TotalGames > 0)
				{
					uint32_t Rating = UTIL_ToUInt32(Row[3]);
					uint32_t RatingPeak = UTIL_ToUInt32(Row[4]);
					uint32_t TotalWins = UTIL_ToUInt32(Row[5]);
					uint32_t TotalLosses = UTIL_ToUInt32(Row[6]);
					uint32_t TotalKills = UTIL_ToUInt32(Row[7]);
					uint32_t TotalDeaths = UTIL_ToUInt32(Row[8]);
					uint32_t TotalAssists = UTIL_ToUInt32(Row[9]);
					uint32_t TotalCreepKills = UTIL_ToUInt32(Row[10]);
					uint32_t TotalCreepDenies = UTIL_ToUInt32(Row[11]);
					uint32_t TotalNeutralKills = UTIL_ToUInt32(Row[12]);
					uint32_t TotalTowerKills = UTIL_ToUInt32(Row[13]);
					uint32_t TotalRaxKills = UTIL_ToUInt32(Row[14]);
					uint32_t TotalCourierKills = UTIL_ToUInt32(Row[15]);
					uint32_t TotalLeaves = UTIL_ToUInt32(Row[16]);
					uint32_t TotalPlayTime = UTIL_ToUInt32(Row[17]);

					uint32_t SeasonGames = TotalWins + TotalLosses;
					SeasonGames = SeasonGames != 0 ? SeasonGames : 1; //prevent division by zero

					double wpg = 0;
					double lpg = 0;
					double kpg = (double)TotalKills / SeasonGames;
					double dpg = (double)TotalDeaths / SeasonGames;
					double ckpg = (double)TotalCreepKills / SeasonGames;
					double cdpg = (double)TotalCreepDenies / SeasonGames;
					double apg = (double)TotalAssists / SeasonGames;
					double nkpg = (double)TotalNeutralKills / SeasonGames;
					double tkpg = (double)TotalTowerKills / SeasonGames;
					double rkpg = (double)TotalRaxKills / SeasonGames;
					double coukpg = (double)TotalCourierKills / SeasonGames;
					double lvpg = (double)TotalLeaves / SeasonGames;
					////////////////////////////////////////////////server = Row[21];
//					double Score = UTIL_ToDouble( Row[21] );
					uint32_t Rank = 0;
					wpg = (double)TotalWins / SeasonGames;
					lpg = (double)TotalLosses / SeasonGames;
					wpg = wpg * 100;
					lpg = lpg * 100;

					DotAPlayerSummary = new CDBDotAPlayerSummaryNew(string(), name, TotalGames, TotalWins, TotalLosses, TotalKills, TotalDeaths, TotalCreepKills, TotalCreepDenies, TotalAssists, TotalNeutralKills, TotalTowerKills, TotalRaxKills, TotalCourierKills, wpg, lpg, kpg, dpg, ckpg, cdpg, apg, nkpg, Score, tkpg, rkpg, coukpg, Rank,
						TotalLeaves, TotalPlayTime, Rating, RatingPeak, lvpg);
				}
			}
			else;
			//				*error = "error checking dotaplayersummary [" + name + "] - row doesn't have 23 columns";

			mysql_free_result(Result);
		}
		else
			*error = mysql_error((MYSQL*)conn);
	}

	return DotAPlayerSummary;
}

CDBDotATopPlayers* MySQLDotATopPlayersQuery(void* conn, string* error, string server, string mingames, uint32_t offset, uint32_t count)
{
	CDBDotATopPlayers *Res = new CDBDotATopPlayers(count);
	string EscServer = MySQLEscapeString(conn, server);
	string Query = "SELECT name,rating FROM `dotaplayerstats` WHERE servername='" + EscServer + "' ORDER BY rating desc LIMIT " + UTIL_ToString(count) + " OFFSET " + UTIL_ToString(offset);

	if (mysql_real_query((MYSQL*)conn, Query.c_str(), Query.size()) != 0)
		*error = mysql_error((MYSQL*)conn);
	else
	{
		MYSQL_RES* Result = mysql_store_result((MYSQL*)conn);
		string PlayersNames[10];
		uint32_t PlayersRatings[10];

		if (Result)
		{
			vector<string> Row = MySQLFetchRow(Result);
			uint32_t i = 0;
			while (i <  Res->GetSize() && !Row.empty())
			{		
				Res->SetlayerName(i, (Row)[0]);
				Res->SetPlayerRating(i, UTIL_ToUInt32((Row)[1]));
				Row = MySQLFetchRow(Result);
				i++;
			}
			mysql_free_result(Result);
			Res->SetCount(i);
		}
		else
			*error = mysql_error((MYSQL*)conn);
	}

	Res->SetOffset(offset);
	return Res;
}

uint32_t MySQLDotAPlayerStatsUpdate(void* conn, string* error, string serverName, string name, CDBDotAPlayer* dotaPlayer, CDBDotAGame* dotaGame, uint32_t baseRating, uint32_t opponentAvgRaing)
{
	uint32_t RowID = 0;
	if (dotaGame && dotaGame->GetWinner() != 0) // If there is a winner
	{
		bool RowExists;
		string SdError = string();
		//m_Result = MySQLDotAPlayerSummaryCheck(m_Connection, &m_Error, m_SQLBotID, m_Name);
		//CDBDotAPlayerSummaryNew *CurrentPlayerSummary = MySQLDotAPlayerSummaryCheckNew(conn, &SdError, name, serverName, "", 0);
		string forforfor = "form";
		CDBDotAPlayerSummaryNew* CurrentPlayerSummary = NULL;
	


		//ThreadedDotAPlayerSummaryCheckNew   LEARN FROM THIS
		/*
		CCallableDotAPlayerSummaryCheckNew* Callable = new CMySQLCallableDotAPlayerSummaryCheckNew(serverName, name, UTIL_ToString(minGames), gamestate, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port);
		CreateThread(Callable);
		//m_Result = MySQLDotAPlayerStatsUpdate(m_Connection, &m_Error, m_ServerName, m_Name, m_DotAPlayer, m_DotAGame, m_BaseRating);
		CGHostDBMySQL;
		CGHostDB db = NULL;
		db.DotAPlayerSummaryCheckNew(name);
		*/

		//======================================================================



		//Now lets retrieve current stats from DB (Copied and pasted from above function)
		//==================================================================
		transform(name.begin(), name.end(), name.begin(), (int(*)(int))tolower);
		string EscName = MySQLEscapeString(conn, name);
		string EscServerName = MySQLEscapeString(conn, serverName);
		uint32_t Rank = 0;
		double Score = -10000;
		string server = string();
		CDBDotAPlayerSummaryNew* DotAPlayerSummary = NULL;
		//	string Query = "select totgames,wins,losses,killstotal,deathstotal,creepkillstotal,creepdeniestotal,assiststotal,neutralkillstotal,towerkillstotal,raxkillstotal,courierkillstotal,kills,deaths,creepkills,creepdenies,assists,neutralkills,towerkills,raxkills,courierkills, ("+formula+") as totalscore,server from(select *, (kills/deaths) as killdeathratio, (totgames-wins) as losses from (select gp.name as name,ga.server as server,gp.gameid as gameid, gp.colour as colour, avg(dp.courierkills) as courierkills, sum(dp.raxkills) as raxkillstotal, sum(dp.towerkills) as towerkillstotal, sum(dp.assists) as assiststotal,sum(dp.courierkills) as courierkillstotal, sum(dp.creepdenies) as creepdeniestotal, sum(dp.creepkills) as creepkillstotal,sum(dp.neutralkills) as neutralkillstotal, sum(dp.deaths) as deathstotal, sum(dp.kills) as killstotal,avg(dp.raxkills) as raxkills,avg(dp.towerkills) as towerkills, avg(dp.assists) as assists, avg(dp.creepdenies) as creepdenies, avg(dp.creepkills) as creepkills,avg(dp.neutralkills) as neutralkills, avg(dp.deaths) as deaths, avg(dp.kills) as kills,count(*) as totgames, SUM(case when((dg.winner = 1 and dp.newcolour < 6) or (dg.winner = 2 and dp.newcolour > 6)) then 1 else 0 end) as wins from gameplayers as gp, dotagames as dg, games as ga,dotaplayers as dp where dg.winner <> 0 and dp.gameid = gp.gameid and dg.gameid = dp.gameid and dp.gameid = ga.id and gp.gameid = dg.gameid and gp.colour = dp.colour and gp.name='"+name+"' group by gp.name) as h) as i";
		string Query = "SELECT name, servername, rating, ratingpeak, games, wins, loses, kills, deaths, assists, creepkills, creepdenies, neutralkills, towerkills, raxkills, courierkills, leaves, playtime, joindate FROM `dotaplayerstats` WHERE `name` = '" + EscName + "' AND `servername` = '" + EscServerName + "' LIMIT 1";
		//				0			1		2		3			4		5	6		7		8		9			10			11			12			13			14			15			16		17			18
		if (mysql_real_query((MYSQL*)conn, Query.c_str(), Query.size()) != 0)
			*error = mysql_error((MYSQL*)conn);
		else
		{
			MYSQL_RES* Result = mysql_store_result((MYSQL*)conn);

			if (Result)
			{
				vector<string> Row = MySQLFetchRow(Result);

				if (Row.size() == 19)
				{
					string ResPlayerName = Row[0];
					string ResServerName = Row[1];
					uint32_t TotalGames = UTIL_ToUInt32(Row[4]);

					if (TotalGames > 0)
					{
						uint32_t Rating = UTIL_ToUInt32(Row[3]);
						uint32_t RatingPeak = UTIL_ToUInt32(Row[4]);
						uint32_t TotalWins = UTIL_ToUInt32(Row[5]);
						uint32_t TotalLosses = UTIL_ToUInt32(Row[6]);
						uint32_t TotalKills = UTIL_ToUInt32(Row[7]);
						uint32_t TotalDeaths = UTIL_ToUInt32(Row[8]);
						uint32_t TotalAssists = UTIL_ToUInt32(Row[9]);
						uint32_t TotalCreepKills = UTIL_ToUInt32(Row[10]);
						uint32_t TotalCreepDenies = UTIL_ToUInt32(Row[11]);
						uint32_t TotalNeutralKills = UTIL_ToUInt32(Row[12]);
						uint32_t TotalTowerKills = UTIL_ToUInt32(Row[13]);
						uint32_t TotalRaxKills = UTIL_ToUInt32(Row[14]);
						uint32_t TotalCourierKills = UTIL_ToUInt32(Row[15]);
						uint32_t TotalLeaves = UTIL_ToUInt32(Row[16]);
						uint32_t TotalPlayTime = UTIL_ToUInt32(Row[17]);

						uint32_t SeasonGames = TotalWins + TotalLosses;
						SeasonGames = SeasonGames != 0 ? SeasonGames : 1; //prevent division by zero

						double wpg = 0;
						double lpg = 0;
						double kpg = (double)TotalKills / SeasonGames;
						double dpg = (double)TotalDeaths / SeasonGames;
						double ckpg = (double)TotalCreepKills / SeasonGames;
						double cdpg = (double)TotalCreepDenies / SeasonGames;
						double apg = (double)TotalAssists / SeasonGames;
						double nkpg = (double)TotalNeutralKills / SeasonGames;
						double tkpg = (double)TotalTowerKills / SeasonGames;
						double rkpg = (double)TotalRaxKills / SeasonGames;
						double coukpg = (double)TotalCourierKills / SeasonGames;
						double lvpg = (double)TotalLeaves / SeasonGames;
						////////////////////////////////////////////////server = Row[21];
	//					double Score = UTIL_ToDouble( Row[21] );
						uint32_t Rank = 0;
						wpg = (double)TotalWins / SeasonGames;
						lpg = (double)TotalLosses / SeasonGames;
						wpg = wpg * 100;
						lpg = lpg * 100;

						DotAPlayerSummary = new CDBDotAPlayerSummaryNew(string(), name, TotalGames, TotalWins, TotalLosses, TotalKills, TotalDeaths, TotalCreepKills, TotalCreepDenies, TotalAssists, TotalNeutralKills, TotalTowerKills, TotalRaxKills, TotalCourierKills, wpg, lpg, kpg, dpg, ckpg, cdpg, apg, nkpg, Score, tkpg, rkpg, coukpg, Rank,
							TotalLeaves, TotalPlayTime, Rating, RatingPeak, lvpg);
					}
				}
				else;
				//				*error = "error checking dotaplayersummary [" + name + "] - row doesn't have 23 columns";

				mysql_free_result(Result);
			}
			else
				*error = mysql_error((MYSQL*)conn);
		}

		//We retrieved current stats
		//=====================================================================================
		CurrentPlayerSummary = DotAPlayerSummary;



		uint32_t minGames = 1;
		//CCallableDotAPlayerSummaryCheckNew* Callable = new CMySQLCallableDotAPlayerSummaryCheckNew(serverName, name, UTIL_ToString(minGames), string(), conn, 0, string(), m_Database, m_User, m_Password, m_Port);


		if (SdError.empty())
		{
			if (!CurrentPlayerSummary)
			{
				RowExists = false;
				CurrentPlayerSummary = new CDBDotAPlayerSummaryNew(serverName, name, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, baseRating, 0, 0);
			}
			else
				RowExists = true;

			int32_t EloWin = 0;
			int32_t EloLoss = 0;
			elorating2::CalculateEloRatingChange(CurrentPlayerSummary->GetRating(), opponentAvgRaing, &EloWin, &EloLoss);
			uint32_t Winner = dotaGame->GetWinner();
			uint32_t PlayerColour = dotaPlayer->GetNewColour();
			bool won = (Winner == 1 && PlayerColour >= 1 && PlayerColour <= 5) || (Winner == 2 && PlayerColour >= 7 && PlayerColour <= 11);		//"' AND ((winner=1 AND dotaplayers.newcolour>=1 AND dotaplayers.newcolour<=5) OR (winner=2 AND dotaplayers.newcolour>=7 AND dotaplayers.newcolour<=11))"
			bool lost = (Winner == 2 && PlayerColour >= 1 && PlayerColour <= 5) || (Winner == 1 && PlayerColour >= 7 && PlayerColour <= 11); 	//		 ((winner=2 AND dotaplayers.newcolour>=1 AND dotaplayers.newcolour<=5) OR (winner=1 AND dotaplayers.newcolour>=7 AND dotaplayers.newcolour<=11))
			uint32_t newRating = CurrentPlayerSummary->GetRating() + 0;
			if (won)
				newRating += EloWin;
			else if(lost)
				newRating += EloLoss;

			CDBDotAPlayerSummaryNew UpdatedPlayerSummary(CurrentPlayerSummary->GetServer(), CurrentPlayerSummary->GetName(), CurrentPlayerSummary->GetTotalGames() + 1,
				CurrentPlayerSummary->GetTotalWins() + (won ? 1 : 0), CurrentPlayerSummary->GetTotalLosses() + (lost ? 1 : 0), CurrentPlayerSummary->GetTotalKills() + dotaPlayer->GetKills(), CurrentPlayerSummary->GetTotalDeaths() + dotaPlayer->GetDeaths(),
				CurrentPlayerSummary->GetTotalCourierKills() + dotaPlayer->GetCreepKills(), CurrentPlayerSummary->GetTotalCreepDenies() + dotaPlayer->GetCreepDenies(), CurrentPlayerSummary->GetTotalAssists() + dotaPlayer->GetAssists(), CurrentPlayerSummary->GetTotalNeutralKills() + dotaPlayer->GetNeutralKills(),
				CurrentPlayerSummary->GetTotalTowerKills() + dotaPlayer->GetTowerKills(), CurrentPlayerSummary->GetTotalRaxKills() + dotaPlayer->GetRaxKills(), CurrentPlayerSummary->GetTotalCourierKills() + dotaPlayer->GetCourierKills(),
				0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, CurrentPlayerSummary->GetTotalLeaves() + 0, CurrentPlayerSummary->GetTotalPlayedMinutes() + dotaGame->GetMin(), newRating, (newRating > CurrentPlayerSummary->GetRatingPeak()? newRating : CurrentPlayerSummary->GetRatingPeak()),0);
	
			string EscServerName = MySQLEscapeString(conn, UpdatedPlayerSummary.GetServer());
			string EscName = MySQLEscapeString(conn, UpdatedPlayerSummary.GetName());
			string Query;

			if (!RowExists)
				Query = "INSERT INTO `dotaplayerstats` (`name`, `servername`, `rating`, `ratingpeak`, `games`, `wins`, `loses`, `kills`, `deaths`, `assists`, `creepkills`, `creepdenies`, `neutralkills`, `towerkills`, `raxkills`, `courierkills`, `leaves`, `playtime`) VALUES ('" + EscName + "', '" + EscServerName + "', " + UTIL_ToString(UpdatedPlayerSummary.GetRating()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetRatingPeak()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalGames()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalWins()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalLosses()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalKills()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalDeaths()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalAssists()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalCreepKills()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalCreepDenies()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalNeutralKills()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalTowerKills()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalRaxKills()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalCourierKills()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalLeaves()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalPlayedMinutes()) + ")";
			else
				Query = "UPDATE `dotaplayerstats` SET `rating` = " + UTIL_ToString(UpdatedPlayerSummary.GetRating()) + ", `ratingpeak` = " + UTIL_ToString(UpdatedPlayerSummary.GetRatingPeak()) + ", `games` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalGames()) +", `wins` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalWins()) +", `loses` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalLosses()) +", `kills` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalKills()) +", `deaths` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalDeaths()) +", `assists` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalAssists()) +", `creepkills` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalCreepKills()) +", `creepdenies` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalCreepDenies()) +", `neutralkills` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalNeutralKills()) +", `towerkills` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalTowerKills()) +", `raxkills` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalRaxKills()) +", `courierkills` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalCourierKills()) +", `leaves` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalLeaves()) + ", `playtime` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalPlayedMinutes()) + " WHERE `dotaplayerstats`.`name` = '" + EscName + "' AND `servername` = '" + EscServerName + "'";
	
			if (mysql_real_query((MYSQL*)conn, Query.c_str(), Query.size()) != 0)
				*error = mysql_error((MYSQL*)conn);
			else
				if (!RowExists)	
					RowID = (uint32_t)mysql_insert_id((MYSQL*)conn);
		}
		else
		{
			//Failed to retrive existing stats
			*error = SdError;
		}
	}
	return RowID;
}

uint32_t MySQLDotAPlayerStatsUpdateTHISTHIS(void* conn, string* error, string serverName, string name, CDBDotAPlayer* dotaPlayer, CDBDotAGame* dotaGame, uint32_t baseRating)
{
	uint32_t RowID = 0;

	bool RowExists;
	string SdError = string();
	//m_Result = MySQLDotAPlayerSummaryCheck(m_Connection, &m_Error, m_SQLBotID, m_Name);
	//CDBDotAPlayerSummaryNew *CurrentPlayerSummary = MySQLDotAPlayerSummaryCheckNew(conn, &SdError, name, serverName, "", 0);
	string forforfor = "form";
	CDBDotAPlayerSummaryNew* CurrentPlayerSummary = MySQLDotAPlayerSummaryCheckNew(conn, &SdError, name, serverName, forforfor, 0);

	

		uint32_t minGames = 1;
	//CCallableDotAPlayerSummaryCheckNew* Callable = new CMySQLCallableDotAPlayerSummaryCheckNew(serverName, name, UTIL_ToString(minGames), string(), conn, 0, string(), m_Database, m_User, m_Password, m_Port);


	if (SdError.empty())
	{
		if (!CurrentPlayerSummary)
		{
			RowExists = false;
			CurrentPlayerSummary = new CDBDotAPlayerSummaryNew(serverName, name, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, baseRating, 0, 0);
		}
		else
			RowExists = true;

		bool won = (dotaGame->GetWinner() == 1 && dotaPlayer->GetNewColour() >= 1 && dotaPlayer->GetNewColour() <= 5) || (dotaGame->GetWinner() == 2 && dotaPlayer->GetNewColour() >= 7 && dotaPlayer->GetNewColour() <= 11);	//"' AND ((winner=1 AND dotaplayers.newcolour>=1 AND dotaplayers.newcolour<=5) OR (winner=2 AND dotaplayers.newcolour>=7 AND dotaplayers.newcolour<=11))"
		bool lost = (dotaGame->GetWinner() == 2 && dotaPlayer->GetNewColour() >= 1 && dotaPlayer->GetNewColour() <= 5) || (dotaGame->GetWinner() == 1 && dotaPlayer->GetNewColour() >= 7 && dotaPlayer->GetNewColour() <= 11); 	//		 ((winner=2 AND dotaplayers.newcolour>=1 AND dotaplayers.newcolour<=5) OR (winner=1 AND dotaplayers.newcolour>=7 AND dotaplayers.newcolour<=11))
		uint32_t newRating = CurrentPlayerSummary->GetRating() + 0;

		CDBDotAPlayerSummaryNew UpdatedPlayerSummary(CurrentPlayerSummary->GetServer(), CurrentPlayerSummary->GetName(), CurrentPlayerSummary->GetTotalGames() + 1,
			CurrentPlayerSummary->GetTotalWins() + (won ? 1 : 0), CurrentPlayerSummary->GetTotalLosses() + (lost ? 1 : 0), CurrentPlayerSummary->GetTotalKills() + dotaPlayer->GetKills(), CurrentPlayerSummary->GetTotalDeaths() + dotaPlayer->GetDeaths(),
			CurrentPlayerSummary->GetTotalCourierKills() + dotaPlayer->GetCreepKills(), CurrentPlayerSummary->GetTotalCreepDenies() + dotaPlayer->GetCreepDenies(), CurrentPlayerSummary->GetTotalAssists() + dotaPlayer->GetAssists(), CurrentPlayerSummary->GetTotalNeutralKills() + dotaPlayer->GetNeutralKills(),
			CurrentPlayerSummary->GetTotalTowerKills() + dotaPlayer->GetTowerKills(), CurrentPlayerSummary->GetTotalRaxKills() + dotaPlayer->GetRaxKills(), CurrentPlayerSummary->GetTotalCourierKills() + dotaPlayer->GetCourierKills(),
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, CurrentPlayerSummary->GetTotalLeaves() + 0, CurrentPlayerSummary->GetTotalPlayedMinutes() + dotaGame->GetMin(), newRating, (newRating > CurrentPlayerSummary->GetRatingPeak() ? newRating : CurrentPlayerSummary->GetRatingPeak()), 0);

		string EscServerName = MySQLEscapeString(conn, UpdatedPlayerSummary.GetServer());
		string EscName = MySQLEscapeString(conn, UpdatedPlayerSummary.GetName());
		string Query;

		if (!RowExists)
			Query = "INSERT INTO `dotaplayerstats` (`name`, `servername`, `rating`, `ratingpeak`, `games`, `wins`, `loses`, `kills`, `deaths`, `assists`, `creepkills`, `creepdenies`, `neutralkills`, `towerkills`, `raxkills`, `courierkills`, `leaves`, `playtime`) VALUES ('" + EscName + "', '" + EscServerName + "', " + UTIL_ToString(UpdatedPlayerSummary.GetRating()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetRatingPeak()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalGames()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalWins()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalLosses()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalKills()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalDeaths()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalAssists()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalCreepKills()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalCreepDenies()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalNeutralKills()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalTowerKills()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalRaxKills()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalCourierKills()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalLeaves()) + ", " + UTIL_ToString(UpdatedPlayerSummary.GetTotalPlayedMinutes()) + ")";
		else
			Query = "UPDATE `dotaplayerstats` SET `rating` = " + UTIL_ToString(UpdatedPlayerSummary.GetRating()) + ", `ratingpeak` = " + UTIL_ToString(UpdatedPlayerSummary.GetRatingPeak()) + ", `games` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalGames()) + ", `wins` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalWins()) + ", `loses` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalLosses()) + ", `kills` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalKills()) + ", `deaths` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalDeaths()) + ", `assists` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalAssists()) + ", `creepkills` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalCreepKills()) + ", `creepdenies` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalCreepDenies()) + ", `neutralkills` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalNeutralKills()) + ", `towerkills` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalTowerKills()) + ", `raxkills` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalRaxKills()) + ", `courierkills` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalCourierKills()) + ", `leaves` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalLeaves()) + ", `playtime` = " + UTIL_ToString(UpdatedPlayerSummary.GetTotalPlayedMinutes()) + " WHERE `dotaplayerstats`.`name` = '" + EscName + "' AND `servername` = '" + EscServerName + "'";

		if (mysql_real_query((MYSQL*)conn, Query.c_str(), Query.size()) != 0)
			*error = mysql_error((MYSQL*)conn);
		else
			if (!RowExists)
				RowID = (uint32_t)mysql_insert_id((MYSQL*)conn);
	}
	else
	{
		//Failed to retrive existing stats
		*error = SdError;
	}

	return RowID;
}

uint32_t MySQLCurrentGameUpdate(void* conn, string* error, uint32_t botid, unsigned char action, string param, string creatorname, string ownername, string gamename, string names, string mapname, string createdat, string startedat, string expiredate, bool gamestarted, uint32_t gamerandomid, bool clearall, uint8_t occupiedslots, uint8_t maxslots)
{
	//action: 0=add/update(replace) 1=delete 2=?
	uint32_t RowID =0;
	string EscCreatorName = MySQLEscapeString(conn, creatorname);
	string EscOwnerName = MySQLEscapeString(conn, ownername);
	string EscGameName = MySQLEscapeString(conn, gamename);
	string EscPlayersNames = MySQLEscapeString(conn, names);
	string EscMapName = MySQLEscapeString(conn, mapname);
	string CreatedAt = !createdat.empty() ? "'" + createdat + "'" : "UTC_TIMESTAMP()";
	string StartedAt = !startedat.empty() ? "'" + startedat + "'" : "UTC_TIMESTAMP()";
	string ExpireDate = !expiredate.empty() ? "'" + expiredate + "'" : "NULL";

	if (clearall)
	{
		string ClearQuery = "DELETE FROM `currentgames` WHERE bot_id = " + UTIL_ToString(botid);
		if (mysql_real_query((MYSQL*)conn, ClearQuery.c_str(), ClearQuery.size()) != 0)
			*error = mysql_error((MYSQL*)conn);
		else
		{
			//Success = true;
		}		
	}

	if (action == 0)
	{
		string ReplaceQuery = "REPLACE INTO `currentgames` (`id`, `bot_id` , `owner_name`, `game_name`, `names`, `map_name`, `created_at`, `updated_at`, `started_at`, `expire_date`, `started`, `occupied_slots`, `max_slots`) VALUES (" + UTIL_ToString(gamerandomid) + ", '" + UTIL_ToString(botid) + "', '" + EscOwnerName + "', '" + EscGameName + "', '" + EscPlayersNames + "', '" + EscMapName + "', " + CreatedAt + ", UTC_TIMESTAMP(), " + StartedAt + ", " + ExpireDate + ", '" + (gamestarted ? "1" : "0") + "', '" + UTIL_ToString(occupiedslots) + "', '" + UTIL_ToString(maxslots) +"') ";
		if (mysql_real_query((MYSQL*)conn, ReplaceQuery.c_str(), ReplaceQuery.size()) != 0)
			*error = mysql_error((MYSQL*)conn);
		else
			RowID = (uint32_t)mysql_insert_id((MYSQL*)conn);
	}
	else if (action == 1)
	{
		string DeleteQuery = "DELETE FROM `currentgames` WHERE `id` = " + UTIL_ToString(gamerandomid);
		if (mysql_real_query((MYSQL*)conn, DeleteQuery.c_str(), DeleteQuery.size()) != 0)
			*error = mysql_error((MYSQL*)conn);
		else
			RowID = (uint32_t)mysql_insert_id((MYSQL*)conn);
	}
	else if (action == 2)
	{
		string AddQuery = "INSERT INTO `currentgames` (`id`, `owner_name`, `game_name`, `names`, `map_name`, `created_at`, `updated_at`, `started_at`, `expire_date`, `started`) VALUES (NULL, '" + EscOwnerName + "', '" + EscGameName + "', '" + EscPlayersNames + "', '" + EscMapName + "', " + CreatedAt + ", NOW(), " + StartedAt + ", " + ExpireDate + ", '" + (gamestarted ? "1" : "0") + "') ";
	}

	if (false && action == 0)
	{ //Update if exists, add if not
		string SelectQuery = "SELECT `id` FROM `currentgames` where `bot_id` = '" + UTIL_ToString(botid) + "' AND `game_random_id` = '" + UTIL_ToString(gamerandomid) + "'";
		if (mysql_real_query((MYSQL*)conn, SelectQuery.c_str(), SelectQuery.size()) != 0)
			*error = mysql_error((MYSQL*)conn);
		else
		{
			MYSQL_RES* SelectResult = mysql_store_result((MYSQL*)conn);
			if (SelectResult && SelectResult->row_count > 0)
			{
				string UpdateQuery = "";
			}
			else
			{
			}
		}
	}

	return RowID;
}

vector<CDBCurrentGame*> MySQLCurrentGamesQuery(void* conn, string* error, bool includelobbies, bool includestarted, uint32_t queryoffset, uint32_t querylimit, uint32_t& outlobbiescount, uint32_t& outgamescount)
{
	vector<CDBCurrentGame*> Res;

	//Clean expired rows

	string DeleteQuery = "DELETE FROM `currentgames` WHERE `expire_date` < UTC_TIMESTAMP";
	if (mysql_real_query((MYSQL*)conn, DeleteQuery.c_str(), DeleteQuery.size()) != 0)
		*error = mysql_error((MYSQL*)conn);

	//find total number of lobbies/games

	if (&outlobbiescount && &outgamescount)
	{
		string CountQuery = "SELECT '0' as started ,COUNT(*) FROM currentgames WHERE started = '0' union SELECT '1' , COUNT(*) FROM currentgames WHERE started = '1'";
		if (mysql_real_query((MYSQL*)conn, CountQuery.c_str(), CountQuery.size()) != 0)
			*error = mysql_error((MYSQL*)conn);
		else
		{
			MYSQL_RES* Result = mysql_store_result((MYSQL*)conn);
			if (Result)
			{
				vector<string> Row = MySQLFetchRow(Result);
				while (Row.size() == 2)
				{
					if (Row[0] == "0")
						outlobbiescount = UTIL_ToInt32(Row[1]);
					else if(Row[0] == "1")
						outgamescount = UTIL_ToInt32(Row[1]);

					Row = MySQLFetchRow(Result);
				}
				mysql_free_result(Result);
			}
			else
				*error = mysql_error((MYSQL*)conn);
		}
	}
	else if (&outlobbiescount)
	{
		string CountQuery = "SELECT COUNT(id) AS NumberOfLobbies FROM `currentgames` WHERE `started` = 0";
		if (mysql_real_query((MYSQL*)conn, CountQuery.c_str(), CountQuery.size()) != 0)
			*error = mysql_error((MYSQL*)conn);
		else
		{
			MYSQL_RES* Result = mysql_store_result((MYSQL*)conn);
			if (Result)
			{
				vector<string> Row = MySQLFetchRow(Result);
				if (!Row.empty())
					outlobbiescount = UTIL_ToUInt32((Row)[0]);
			}
			else
				*error = mysql_error((MYSQL*)conn);
		}
	}
	else if (&outgamescount)
	{
		string CountQuery = "SELECT COUNT(id) AS NumberOfGames FROM `currentgames` WHERE `started` = 1";
		if (mysql_real_query((MYSQL*)conn, CountQuery.c_str(), CountQuery.size()) != 0)
			*error = mysql_error((MYSQL*)conn);
		else
		{
			MYSQL_RES* Result = mysql_store_result((MYSQL*)conn);
			if (Result)
			{
				vector<string> Row = MySQLFetchRow(Result);
				if (!Row.empty())
					outgamescount = UTIL_ToUInt32((Row)[0]);
			}
			else
				*error = mysql_error((MYSQL*)conn);
		}
	}

	//Retrieve current games info

	if (querylimit > 0)
	{
		string Query = "SELECT `id`, `bot_id`, `owner_name`, `game_name`, `names`, `map_name`, unix_timestamp(`created_at`), unix_timestamp(`started_at`), unix_timestamp(`expire_date`), `occupied_slots`, `max_slots`, `started` FROM `currentgames`";
		//						0		1			2			3			 4			5			6				7			 8				9				10			 11			
		if (!includelobbies || !includestarted)
			if (includestarted)
				Query += " WHERE `started` = 1";
			else
				Query += " WHERE `started` = 0";
		Query += " ORDER BY `created_at` ASC LIMIT " + UTIL_ToString(querylimit) + " OFFSET " + UTIL_ToString(queryoffset);
		
		if (mysql_real_query((MYSQL*)conn, Query.c_str(), Query.size()) != 0)
			*error = mysql_error((MYSQL*)conn);
		else
		{
			MYSQL_RES* Result = mysql_store_result((MYSQL*)conn);
			if (Result)
			{
				vector<string> Row = MySQLFetchRow(Result);
				while (Row.size() >= 12)
				{
					CDBCurrentGame* CurrGame = new CDBCurrentGame;
					CurrGame ->m_GameRandomID = UTIL_ToInt32(Row[0]);
					CurrGame ->m_BotID = UTIL_ToInt32(Row[1]);
					CurrGame ->m_OwnerName = Row[2];
					CurrGame ->m_GameName = Row[3];
					CurrGame ->m_Names = Row[4];
					CurrGame ->m_MapName = Row[5];			
					CurrGame ->m_CreatedAt = UTIL_ToInt32(Row[6]);
					CurrGame ->m_StartedAt = UTIL_ToInt32(Row[7]);
					CurrGame ->m_ExpireDate = UTIL_ToInt32(Row[8]);
					CurrGame ->m_OccupiedSlots = UTIL_ToInt32(Row[9]);
					CurrGame ->m_MaxSlots = UTIL_ToInt32(Row[10]);
					CurrGame ->m_GameStarted = Row[11] == "0"? false : true;
					Res.push_back(CurrGame);
					Row = MySQLFetchRow(Result);
				}
				mysql_free_result(Result);
			}
			else
				*error = mysql_error((MYSQL*)conn);
		}
	}


	return Res;
}

///////////
void CMySQLCallableDotAPlayerAddNew :: operator( )()
{
	Init();

	if (m_Error.empty())
		m_Result = MySQLDotAPlayerAddNew(m_Connection, &m_Error, m_SQLBotID, m_GameID, m_Colour, m_Kills, m_Deaths, m_CreepKills, m_CreepDenies, m_Assists, m_Gold, m_NeutralKills, m_Item1, m_Item2, m_Item3, m_Item4, m_Item5, m_Item6, m_Hero, m_NewColour, m_TowerKills, m_RaxKills, m_CourierKills);

	Close();
}

void CMySQLCallableDotAPlayerSummaryCheckNew :: operator( )()
{
	Init();

	m_Result = MySQLDotAPlayerSummaryCheckNew(m_Connection, &m_Error, m_Name, m_Server, "it was: m_Formula", m_MinGames);  //was : ( m_Connection, &m_Error, m_SQLBotID, m_Name, m_Formula, m_MinGames, m_GameState )
	if (m_Error.empty())
		m_Result = MySQLDotAPlayerSummaryCheckNew(m_Connection, &m_Error, m_Name, m_Server, "it was: m_Formula", m_MinGames);  //mby this is correct!!!!!!!!
	//m_Result = MySQLDotAPlayerSummaryCheckNew( m_Connection, &m_Error, m_SQLBotID, m_Server,  m_Name, m_MinGames, m_GameState ); //???

	Close();
}

void CMySQLCallableDotATopPlayersQuery :: operator( )()
{
	Init();

	m_Result = MySQLDotATopPlayersQuery(m_Connection, &m_Error, m_Server, m_MinGames, m_Offset, m_Count);
	if (m_Error.empty())
		m_Result = MySQLDotATopPlayersQuery(m_Connection, &m_Error, m_Server, m_MinGames, m_Offset, m_Count); 
	
	Close();
}

void CMySQLCallableDotAPlayerStatsUpdate :: operator( )()
{
	Init();

	if (m_Error.empty())
		m_Result = MySQLDotAPlayerStatsUpdate(m_Connection, &m_Error, m_ServerName, m_Name,m_DotAPlayer,m_DotAGame, m_BaseRating, m_OpponentAvgRaing);
	delete m_DotAPlayer;
	delete m_DotAGame;

	Close();
	
}

void CMySQLCallableCurrentGameUpdate :: operator( )()
{
	Init();

	if (m_Error.empty())
		m_Result = MySQLCurrentGameUpdate(m_Connection, &m_Error, m_BotID, m_Action, m_Param, m_CreatorName, m_OwnerName, m_GameName, m_Names, m_MapName, m_CreatedAt, m_StartedAt, m_ExpireDate, m_GameStarted, m_GameRandomID, m_ClearAll, m_OccupiedSlots, m_MaxSlots);

	Close();

}

void CMySQLCallableCurrentGamesQuery :: operator( )()
{
	Init();

	if (m_Error.empty())
		m_Result = MySQLCurrentGamesQuery(m_Connection, &m_Error, m_IncludeLobbies, m_IncludeStarted, m_QueryOffset, m_QueryLimit, m_TotalLobbyCount, m_TotalGameCount);

	Close();

}
///////////

//incomplete
CCallableDotAPlayerAddNew* CGHostDBMySQL::ThreadedDotAPlayerAddNew(uint32_t gameid, uint32_t colour, uint32_t kills, uint32_t deaths, uint32_t creepkills, uint32_t creepdenies, uint32_t assists, uint32_t gold, uint32_t neutralkills, string item1, string item2, string item3, string item4, string item5, string item6, string hero, uint32_t newcolour, uint32_t towerkills, uint32_t raxkills, uint32_t courierkills)
{
	void* Connection = GetIdleConnection();

	if (!Connection)
		m_NumConnections++;

	CCallableDotAPlayerAddNew* Callable = new CMySQLCallableDotAPlayerAddNew(gameid, colour, kills, deaths, creepkills, creepdenies, assists, gold, neutralkills, item1, item2, item3, item4, item5, item6, hero, newcolour, towerkills, raxkills, courierkills, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port);
	CreateThread(Callable);
	m_OutstandingCallables++;
	return Callable;
}

CCallableDotAPlayerSummaryCheckNew* CGHostDBMySQL::ThreadedDotAPlayerSummaryCheckNew(string servername, string name, string mingames, string gamestate)
{
	void* Connection = GetIdleConnection();

	if (!Connection)
		m_NumConnections++;

	uint32_t minGames = 1;
	CCallableDotAPlayerSummaryCheckNew* Callable = new CMySQLCallableDotAPlayerSummaryCheckNew(servername, name, UTIL_ToString(minGames), gamestate, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port);
	CreateThread(Callable);
	m_OutstandingCallables++;
	return Callable;
}

CCallableDotATopPlayersQuery* CGHostDBMySQL::ThreadedDotATopPlayersQuery(string server, string mingames, uint32_t offset, uint32_t count)
{
	void* Connection = GetIdleConnection();

	if (!Connection)
		m_NumConnections++;

	uint32_t minGames = 1;
	CCallableDotATopPlayersQuery* Callable = new CMySQLCallableDotATopPlayersQuery(server, mingames, offset,count, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port);
	CreateThread(Callable);
	m_OutstandingCallables++;
	return Callable;
}

CCallableDotAPlayerStatsUpdate* CGHostDBMySQL::ThreadedDotAPlayerStatsUpdate(string nServerName, string nName, CDBDotAPlayer* nDotAPlayer, CDBDotAGame* nDotAGame, uint32_t nBaseRating, uint32_t nOpponentAvgRaing)
{
	void* Connection = GetIdleConnection();

	if (!Connection)
		m_NumConnections++;

	CCallableDotAPlayerStatsUpdate* Callable = new CMySQLCallableDotAPlayerStatsUpdate(nServerName,nName,nDotAPlayer,nDotAGame,nBaseRating, nOpponentAvgRaing, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port);
	CreateThread(Callable);
	m_OutstandingCallables++;
	return Callable;
}

CCallableCurrentGameUpdate* CGHostDBMySQL::ThreadedCurrentGameUpdate(uint32_t nBotID, unsigned char nAction, string nParam, string nCreatorName, string nOwnerName, string nGameName, string nNames, string nMapName, string nCreatedAt, string nStartedAt, string nExpireDate, bool nGameStarted, uint32_t nGameRandomID, bool nClearAll, uint8_t nOccupiedSlots, uint8_t nMaxSlots)
{
	void* Connection = GetIdleConnection();

	if (!Connection)
		m_NumConnections++;

	CCallableCurrentGameUpdate* Callable = new CMySQLCallableCurrentGameUpdate(nBotID, nAction, nParam, nCreatorName, nOwnerName, nGameName, nNames, nMapName, nCreatedAt, nStartedAt, nExpireDate, nGameStarted, nGameRandomID, nClearAll, nOccupiedSlots, nMaxSlots, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port);
	CreateThread(Callable);
	m_OutstandingCallables++;
	return Callable;
}

CCallableCurrentGamesQuery* CGHostDBMySQL::ThreadedCurrentGamesQuery(bool includelobbies, bool includestarted, uint32_t queryoffset, uint32_t querylimit)
{
	void* Connection = GetIdleConnection();

	if (!Connection)
		m_NumConnections++;

	CCallableCurrentGamesQuery* Callable = new CMySQLCallableCurrentGamesQuery(includelobbies, includestarted, queryoffset, querylimit, Connection, m_BotID, m_Server, m_Database, m_User, m_Password, m_Port);
	CreateThread(Callable);
	m_OutstandingCallables++;
	return Callable;
}

#endif
