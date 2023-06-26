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

#include "ghost.h"
#include "util.h"
#include "config.h"
#include "language.h"
#include "socket.h"
#include "ghostdb.h"
#include "bnet.h"
#include "map.h"
#include "packed.h"
#include "savegame.h"
#include "gameplayer.h"
#include "gameprotocol.h"
#include "game_base.h"
#include "game.h"
#include "stats.h"
#include "statsdota.h"
#include "statsw3mmd.h"

#include <cmath>
#include <string.h>
#include <time.h>

//
// sorting classes
//

class CGamePlayerSortAscByPing
{
public:
	bool operator( ) ( CGamePlayer *Player1, CGamePlayer *Player2 ) const
	{
		return Player1->GetPing( false ) < Player2->GetPing( false );
	}
};

class CGamePlayerSortDescByPing
{
public:
	bool operator( ) ( CGamePlayer *Player1, CGamePlayer *Player2 ) const
	{
		return Player1->GetPing( false ) > Player2->GetPing( false );
	}
};

class CGamePlayerSortDescByRating //New
{
public:
	bool operator( ) (CGamePlayer* Player1, CGamePlayer* Player2) const
	{
		//return Player1->GetPing(false) > Player2->GetPing(false);
		return Player1->GetDotARating() > Player2->GetDotARating();
	}
};


//
// CGame
//

CGame :: CGame( CGHost *nGHost, CMap *nMap, CSaveGame *nSaveGame, uint16_t nHostPort, unsigned char nGameState, string nGameName, string nOwnerName, string nCreatorName, string nCreatorServer ) : CBaseGame( nGHost, nMap, nSaveGame, nHostPort, nGameState, nGameName, nOwnerName, nCreatorName, nCreatorServer ), m_DBBanLast( NULL ), m_Stats( NULL ) ,m_CallableGameAdd( NULL )
{
	m_DBGame = new CDBGame( 0, string( ), m_Map->GetMapPath( ), string( ), string( ), string( ), 0 );

	if( m_Map->GetMapType( ) == "w3mmd" )
		m_Stats = new CStatsW3MMD( this, m_Map->GetMapStatsW3MMDCategory( ) );
	else if( m_Map->GetMapType( ) == "dota" )
		m_Stats = new CStatsDOTA( this );

	m_GameLoadedTime = 0;
}

CGame :: ~CGame( )
{
	boost::mutex::scoped_lock callablesLock( m_GHost->m_CallablesMutex );
	
	//Ban leavers
	uint32_t timehasleft;
	uint32_t endtime = m_GameOverTime;
	if (endtime == 0)
		endtime = GetTime();
	for (vector<CDBGamePlayer*> ::iterator i = m_DBGamePlayers.begin(); i != m_DBGamePlayers.end(); i++) {
		if (IsAutoBanned((*i)->GetName()))
		{
			uint32_t m_AutoBanGameEndMins = 0; // this should be a cfg variable
			timehasleft = 0; //(*i)->GetLeavingTime();
			if (endtime > timehasleft + m_AutoBanGameEndMins * 60)
			{
				string Reason = " Autobanned, left game \"" +m_GameName + "\"";
				CONSOLE_Print("[AUTOBAN: " + m_GameName + "] Autobanning " + (*i)->GetName() + " (" + Reason + ")");

				m_GHost->m_Callables.push_back( m_GHost->m_DB->ThreadedBanAdd( (*i)->GetSpoofedRealm(), (*i)->GetName( ), (*i)->GetIP(), m_GameName, "AUTOBAN", Reason ));
				//New:
				//m_GHost->m_Callables.push_back(m_GHost->m_DB->ThreadedBanAdd((*i)->GetSpoofedRealm(), (*i)->GetName(), (*i)->GetIP(), m_GameName, "AUTOBAN", Reason, 5, 0));
			}
		}
	}

	//Add game
	if( m_CallableGameAdd && m_CallableGameAdd->GetReady( ) )
	{
		if( m_CallableGameAdd->GetResult( ) > 0 )
		{
			CONSOLE_Print( "[GAME: " + m_GameName + "] saving player/stats data to database" );

			// store the CDBGamePlayers in the database

			for( vector<CDBGamePlayer *> :: iterator i = m_DBGamePlayers.begin( ); i != m_DBGamePlayers.end( ); ++i )
				m_GHost->m_Callables.push_back( m_GHost->m_DB->ThreadedGamePlayerAdd( m_CallableGameAdd->GetResult( ), (*i)->GetName( ), (*i)->GetIP( ), (*i)->GetSpoofed( ), (*i)->GetSpoofedRealm( ), (*i)->GetReserved( ), (*i)->GetLoadingTime( ), (*i)->GetLeft( ), (*i)->GetLeftReason( ), (*i)->GetTeam( ), (*i)->GetColour( ) ) );

			// store the stats in the database

			if( m_Stats )
				m_Stats->Save( m_GHost, m_GHost->m_DB, m_CallableGameAdd->GetResult( ) );
		}
		else
			CONSOLE_Print( "[GAME: " + m_GameName + "] unable to save player/stats data to database" );

		m_GHost->m_DB->RecoverCallable( m_CallableGameAdd );
		delete m_CallableGameAdd;
		m_CallableGameAdd = NULL;
	}

	for( vector<PairedBanCheck> :: iterator i = m_PairedBanChecks.begin( ); i != m_PairedBanChecks.end( ); ++i )
		m_GHost->m_Callables.push_back( i->second );

	for( vector<PairedBanAdd> :: iterator i = m_PairedBanAdds.begin( ); i != m_PairedBanAdds.end( ); ++i )
		m_GHost->m_Callables.push_back( i->second );

	for( vector<PairedGPSCheck> :: iterator i = m_PairedGPSChecks.begin( ); i != m_PairedGPSChecks.end( ); ++i )
		m_GHost->m_Callables.push_back( i->second );

	for( vector<PairedDPSCheck> :: iterator i = m_PairedDPSChecks.begin( ); i != m_PairedDPSChecks.end( ); ++i )
		m_GHost->m_Callables.push_back( i->second );

	for (vector<PairedDPSCheckNew> ::iterator i = m_PairedDPSChecksNew.begin(); i != m_PairedDPSChecksNew.end(); i++)
		m_GHost->m_Callables.push_back(i->second);

	callablesLock.unlock( );

	for( vector<CDBBan *> :: iterator i = m_DBBans.begin( ); i != m_DBBans.end( ); ++i )
		delete *i;

	delete m_DBGame;

	for( vector<CDBGamePlayer *> :: iterator i = m_DBGamePlayers.begin( ); i != m_DBGamePlayers.end( ); ++i )
		delete *i;

	delete m_Stats;

	// it's a "bad thing" if m_CallableGameAdd is non NULL here
	// it means the game is being deleted after m_CallableGameAdd was created (the first step to saving the game data) but before the associated thread terminated
	// rather than failing horribly we choose to allow the thread to complete in the orphaned callables list but step 2 will never be completed
	// so this will create a game entry in the database without any gameplayers and/or DotA stats

	if( m_CallableGameAdd )
	{
		CONSOLE_Print( "[GAME: " + m_GameName + "] game is being deleted before all game data was saved, game data has been lost" );
		boost::mutex::scoped_lock lock( m_GHost->m_CallablesMutex );
		m_GHost->m_Callables.push_back( m_CallableGameAdd );
		lock.unlock( );
	}
}

bool CGame :: Update( void *fd, void *send_fd )
{
	// update callables

	for( vector<PairedBanCheck> :: iterator i = m_PairedBanChecks.begin( ); i != m_PairedBanChecks.end( ); )
	{
		if( i->second->GetReady( ) )
		{
			CDBBan *Ban = i->second->GetResult( );

			if( Ban )
				SendAllChat( m_GHost->m_Language->UserWasBannedOnByBecause( i->second->GetServer( ), i->second->GetUser( ), Ban->GetDate( ), Ban->GetAdmin( ), Ban->GetReason( ) ) );
			else
				SendAllChat( m_GHost->m_Language->UserIsNotBanned( i->second->GetServer( ), i->second->GetUser( ) ) );

			m_GHost->m_DB->RecoverCallable( i->second );
			delete i->second;
			i = m_PairedBanChecks.erase( i );
		}
		else
			++i;
	}

	for( vector<PairedBanAdd> :: iterator i = m_PairedBanAdds.begin( ); i != m_PairedBanAdds.end( ); )
	{
		if( i->second->GetReady( ) )
		{
			if( i->second->GetResult( ) )
			{
				for( vector<CBNET *> :: iterator j = m_GHost->m_BNETs.begin( ); j != m_GHost->m_BNETs.end( ); ++j )
				{
					if( (*j)->GetServer( ) == i->second->GetServer( ) )
						(*j)->AddBan( i->second->GetUser( ), i->second->GetIP( ), i->second->GetGameName( ), i->second->GetAdmin( ), i->second->GetReason( ) );
				}

				SendAllChat( m_GHost->m_Language->PlayerWasBannedByPlayer( i->second->GetServer( ), i->second->GetUser( ), i->first ) );
			}

			m_GHost->m_DB->RecoverCallable( i->second );
			delete i->second;
			i = m_PairedBanAdds.erase( i );
		}
		else
			++i;
	}

	for( vector<PairedGPSCheck> :: iterator i = m_PairedGPSChecks.begin( ); i != m_PairedGPSChecks.end( ); )
	{
		if( i->second->GetReady( ) )
		{
			CDBGamePlayerSummary *GamePlayerSummary = i->second->GetResult( );

			if( GamePlayerSummary )
			{
				if( i->first.empty( ) )
					SendAllChat( m_GHost->m_Language->HasPlayedGamesWithThisBot( i->second->GetName( ), GamePlayerSummary->GetFirstGameDateTime( ), GamePlayerSummary->GetLastGameDateTime( ), UTIL_ToString( GamePlayerSummary->GetTotalGames( ) ), UTIL_ToString( (float)GamePlayerSummary->GetAvgLoadingTime( ) / 1000, 2 ), UTIL_ToString( GamePlayerSummary->GetAvgLeftPercent( ) ) ) );
				else
				{
					CGamePlayer *Player = GetPlayerFromName( i->first, true );

					if( Player )
						SendChat( Player, m_GHost->m_Language->HasPlayedGamesWithThisBot( i->second->GetName( ), GamePlayerSummary->GetFirstGameDateTime( ), GamePlayerSummary->GetLastGameDateTime( ), UTIL_ToString( GamePlayerSummary->GetTotalGames( ) ), UTIL_ToString( (float)GamePlayerSummary->GetAvgLoadingTime( ) / 1000, 2 ), UTIL_ToString( GamePlayerSummary->GetAvgLeftPercent( ) ) ) );
				}
			}
			else
			{
				if( i->first.empty( ) )
					SendAllChat( m_GHost->m_Language->HasntPlayedGamesWithThisBot( i->second->GetName( ) ) );
				else
				{
					CGamePlayer *Player = GetPlayerFromName( i->first, true );

					if( Player )
						SendChat( Player, m_GHost->m_Language->HasntPlayedGamesWithThisBot( i->second->GetName( ) ) );
				}
			}

			m_GHost->m_DB->RecoverCallable( i->second );
			delete i->second;
			i = m_PairedGPSChecks.erase( i );
		}
		else
			++i;
	}

	for( vector<PairedDPSCheck> :: iterator i = m_PairedDPSChecks.begin( ); i != m_PairedDPSChecks.end( ); )
	{
		if( i->second->GetReady( ) )
		{
			CDBDotAPlayerSummary *DotAPlayerSummary = i->second->GetResult( );

			if( DotAPlayerSummary )
			{
				string Summary = m_GHost->m_Language->HasPlayedDotAGamesWithThisBot(	i->second->GetName( ),
					UTIL_ToString( DotAPlayerSummary->GetTotalGames( ) ),
					UTIL_ToString( DotAPlayerSummary->GetTotalWins( ) ),
					UTIL_ToString( DotAPlayerSummary->GetTotalLosses( ) ),
					UTIL_ToString( DotAPlayerSummary->GetTotalKills( ) ),
					UTIL_ToString( DotAPlayerSummary->GetTotalDeaths( ) ),
					UTIL_ToString( DotAPlayerSummary->GetTotalCreepKills( ) ),
					UTIL_ToString( DotAPlayerSummary->GetTotalCreepDenies( ) ),
					UTIL_ToString( DotAPlayerSummary->GetTotalAssists( ) ),
					UTIL_ToString( DotAPlayerSummary->GetTotalNeutralKills( ) ),
					UTIL_ToString( DotAPlayerSummary->GetTotalTowerKills( ) ),
					UTIL_ToString( DotAPlayerSummary->GetTotalRaxKills( ) ),
					UTIL_ToString( DotAPlayerSummary->GetTotalCourierKills( ) ),
					UTIL_ToString( DotAPlayerSummary->GetAvgKills( ), 2 ),
					UTIL_ToString( DotAPlayerSummary->GetAvgDeaths( ), 2 ),
					UTIL_ToString( DotAPlayerSummary->GetAvgCreepKills( ), 2 ),
					UTIL_ToString( DotAPlayerSummary->GetAvgCreepDenies( ), 2 ),
					UTIL_ToString( DotAPlayerSummary->GetAvgAssists( ), 2 ),
					UTIL_ToString( DotAPlayerSummary->GetAvgNeutralKills( ), 2 ),
					UTIL_ToString( DotAPlayerSummary->GetAvgTowerKills( ), 2 ),
					UTIL_ToString( DotAPlayerSummary->GetAvgRaxKills( ), 2 ),
					UTIL_ToString( DotAPlayerSummary->GetAvgCourierKills( ), 2 ) );

				if( i->first.empty( ) )
					SendAllChat( Summary );
				else
				{
					CGamePlayer *Player = GetPlayerFromName( i->first, true );

					if( Player )
						SendChat( Player, Summary );
				}
			}
			else
			{
				if( i->first.empty( ) )
					SendAllChat( m_GHost->m_Language->HasntPlayedDotAGamesWithThisBot( i->second->GetName( ) ) );
				else
				{
					CGamePlayer *Player = GetPlayerFromName( i->first, true );

					if( Player )
						SendChat( Player, m_GHost->m_Language->HasntPlayedDotAGamesWithThisBot( i->second->GetName( ) ) );
				}
			}

			m_GHost->m_DB->RecoverCallable( i->second );
			delete i->second;
			i = m_PairedDPSChecks.erase( i );
		}
		else
			++i;
	}

	for (vector<PairedDPSCheckNew> ::iterator i = m_PairedDPSChecksNew.begin(); i != m_PairedDPSChecksNew.end(); )
	{
		if (i->second->GetReady())
		{
			CDBDotAPlayerSummaryNew* DotAPlayerSummary = i->second->GetResult();

			bool sd = false;
			bool Whisper = !i->first.empty();
			string name = i->first;

			if (i->first[0] == '%')
			{
				name = i->first.substr(1, i->first.length() - 1);
				Whisper = i->first.length() > 1;
				sd = true;
			}

			if (sd)
				if (DotAPlayerSummary)
				{
					uint32_t scorescount = 99; //= m_GHost->ScoresCount();

					CGamePlayer* PlayerN = GetPlayerFromName(i->second->GetName(), true);

					if (PlayerN)
					{
						//PlayerN->SetScoreS(UTIL_ToString2(DotAPlayerSummary->GetScore()));
						//PlayerN->SetRankS(UTIL_ToString(DotAPlayerSummary->GetRank()));
						PlayerN->SetDotASummary(DotAPlayerSummary);
					}

					string RankS = UTIL_ToString(DotAPlayerSummary->GetRank());
					if (DotAPlayerSummary->GetRank() > 0)
						RankS = RankS + "/" + UTIL_ToString(scorescount);

					string Summary = "[" + i->second->GetName() + "] PSR: " + UTIL_ToString(DotAPlayerSummary->GetRating()) + " Games: " + UTIL_ToString(DotAPlayerSummary->GetTotalGames()) +
						" (W/L: " + UTIL_ToString(DotAPlayerSummary->GetTotalWins()) + "/" + UTIL_ToString(DotAPlayerSummary->GetTotalLosses()) + ") " +
						" KDA: " + UTIL_ToString(DotAPlayerSummary->GetAvgKills(),2) + "/" + UTIL_ToString(DotAPlayerSummary->GetAvgDeaths(),2) + "/" + UTIL_ToString(DotAPlayerSummary->GetAvgAssists(),2) +
						" Creep KDN: " + UTIL_ToString(DotAPlayerSummary->GetAvgCreepKills(), 2) + "/" + UTIL_ToString(DotAPlayerSummary->GetAvgCreepDenies(),2) + "/" + UTIL_ToString(DotAPlayerSummary->GetAvgNeutralKills(),2);

					if (!Whisper)
						SendAllChat(Summary);
					else
					{
						CGamePlayer* Player = GetPlayerFromName(i->first, true);

						if (Player)
							SendChat(Player, Summary);
					}
				}
				else
				{
					SendAllChat(m_GHost->m_Language->HasntPlayedDotAGamesWithThisBot(i->second->GetName()));
				}
			if (!sd)
				if (DotAPlayerSummary)
				{
					string Summary = "[" + i->second->GetName() + "] PSR: " + UTIL_ToString(DotAPlayerSummary->GetRating()) + " Games: " + UTIL_ToString(DotAPlayerSummary->GetTotalGames()) +
						" (W/L: " + UTIL_ToString(DotAPlayerSummary->GetTotalWins()) + "/" + UTIL_ToString(DotAPlayerSummary->GetTotalLosses()) + ") " +
						" KDA: " + UTIL_ToString(DotAPlayerSummary->GetAvgKills(),2) + "/" + UTIL_ToString(DotAPlayerSummary->GetAvgDeaths(),2) + "/" + UTIL_ToString(DotAPlayerSummary->GetAvgAssists(),2) +
						" Creep KDN: " + UTIL_ToString(DotAPlayerSummary->GetAvgCreepKills(),2) + "/" + UTIL_ToString(DotAPlayerSummary->GetAvgCreepDenies(),2) + "/" + UTIL_ToString(DotAPlayerSummary->GetAvgNeutralKills(),2);

					if (i->first.empty())
						SendAllChat(Summary);
					else
					{
						CGamePlayer* Player = GetPlayerFromName(i->first, true);

						if (Player)
							SendChat(Player, Summary);
					}
				}
				else
				{
					if (i->first.empty())
						SendAllChat(m_GHost->m_Language->HasntPlayedDotAGamesWithThisBot(i->second->GetName()));
					else
					{
						CGamePlayer* Player = GetPlayerFromName(i->first, true);

						if (Player)
							SendChat(Player, m_GHost->m_Language->HasntPlayedDotAGamesWithThisBot(i->second->GetName()));
					}
				}

			m_GHost->m_DB->RecoverCallable(i->second);
			delete i->second;
			i = m_PairedDPSChecksNew.erase(i);
		}
		else
			i++;
	}

	return CBaseGame :: Update( fd, send_fd );
}

// this function is only called when a player leave packet is received, not when there's a socket error, kick, etc...
void CGame::EventPlayerLeft(CGamePlayer* player, uint32_t reason)
{
	//we need to add these variables to config!!! --------------------------------------------
	m_GHost->m_AutoBan = true;							// if we have auto ban on by default or not	
	m_GHost->m_AutoBanTeamDiffMax = 2;			// if we have more then x number of players more then other team
	m_GHost->m_AutoBanTimer = 120;				// time in mins the auto ban will stay on in game.
	m_GHost->m_AutoBanAll = true;						// ban even if it does not make game uneven
	m_GHost->m_AutoBanFirstXLeavers = 2;		// bans the first x leavers reguardless of even or not.
	m_GHost->m_AutoBanGameLoading = false;			// Ban if leave during loadin
	m_GHost->m_AutoBanCountDown = false;			// Ban if leave during game start countdown.
	m_GHost->m_AutoBanGameEndMins = 0;			// Ban if not left around x mins of game end time.
	string m_GetMapType = "dota";
	//---------------------------------------------------------------------------------------
	bool useGhostOneAutoBanAlgorithm = false;
	uint32_t maxAutoBanPlayers = 3;
	 //---------------------------------------------------------------------------------------

	// Check if leaver is admin/root admin with a loop then set the bool accordingly.
	bool isAdmin = false;
	for (vector<CBNET*> ::iterator j = m_GHost->m_BNETs.begin(); j != m_GHost->m_BNETs.end(); j++)
	{
		if ((*j)->IsAdmin(player->GetName()) || (*j)->IsRootAdmin(player->GetName()))
		{
			isAdmin = true;
			break;
		}
	}

	isAdmin = false; // ban admins for testing reasons <-------------delete this!!!!!!!!!!!!!!!!!!!!!!!

	// Auto Ban (if m_AutoBan is set to 1)
	// Current Triggers : Map is two team, even playered, is not a admin, game ended timer not started, game has loaded, leaver makes game uneven and conditions are met OR game is loading
	// Start with not banning the player (innocent until proven guilty lol)
	m_BanOn = false;

	if (!useGhostOneAutoBanAlgorithm) 
	{ //Similar to Lagabuse's algorithm
		if (m_AutoBanState)
		{
			if (!isAdmin) {
				if (m_GameLoaded && !m_GameEnded) {
					m_BanOn = true;
				}
				else if (m_GameLoading && m_GHost->m_AutoBanGameLoading) {
					// If game is loading and player is not a admin ban.
					m_BanOn = true;
				}
				else if (m_CountDownStarted && !m_GameLoading && !m_GameLoaded && m_GHost->m_AutoBanCountDown) {
					// If game is loading and player is not a admin ban
					m_BanOn = true;
				}
			}

			if (m_GameLoaded && !m_GameEnded) {
				ReCalculateTeams();

				//Disable auto ban and possibly drop DotA ladder game
				float iTime = (float)(GetTime() - m_GameLoadedTime);
				int dropTime1 = 7 * 60, dropTime2 = 17 * 60; // if a player leaves before dropTime1 or two players from the same team leave before dropTime2, Go RMK

				if ((m_PlayersLeft == 0 && iTime <= dropTime1) || (m_PlayersLeft > 0 && iTime > dropTime1 && iTime < dropTime2 && m_TeamDiff > 0)) {
					//Drop DotA ladder game
					m_AutoBanState = false;
					m_IsLadderGame = false;
					//..
					//..
					SendAllChat("Auto ban OFF, this game will not be saved to your ladder stats");
					StopPlayers("was disconnected (game dropped by early leaver)");
				}

				if (m_PlayersLeft + 1 >= maxAutoBanPlayers) {
					m_AutoBanState = false;
					SendAllChat("[BOT] This game will be saved to your ladder stats, you should play till end");
				}
			}

		}

	}
	else 
	{
		if (m_GHost->m_AutoBan && !isAdmin) {
			// Check if the game has loaded + is not a admin + has not ended + bot_autoban = 1
			if (m_GameLoaded && !m_GameEnded) {
				// If m_AutoBanAll is on
				if (m_GHost->m_AutoBanAll) { m_BanOn = true; }
				// If there is even amount of players on the two teamed map loaded
				if (m_EvenPlayeredTeams) {
					// first check the teams
					ReCalculateTeams();
					// set up the player's SID
					unsigned char SID = GetSIDFromPID(player->GetPID());
					unsigned char fteam;
					if (SID == 255) {
						CBaseGame::EventPlayerLeft(player, reason);
						return;
					}
					fteam = m_Slots[SID].GetTeam();
					// If team is even then turn on auto ban
					if (m_TeamDiff == 0) { m_BanOn = true; }
					// Else if teams are uneven check which one it is. Then depending on the team that is uneven, check to see if we should ban the leaver for making it uneven or more uneven.
					else if (m_TeamDiff > 0)
					{
						// If leaver is on team one then check if it is the team with less players.
						if (fteam == 0) {
							// If it is then turn on Auto Ban.
							if (m_Team1 < m_Team2) { m_BanOn = true; }
						}
						// If leaver is on team two then check if it is the team with less players.
						else if (fteam == 1) {
							// If it is then turn on Auto Ban.
							if (m_Team2 < m_Team1) { m_BanOn = true; }
						}
					}
					// If m_AutoBanTeamDiffMax is set to something other than 0 and m_TeamDiff is greater than m_AutoBanTeamDiffMax. All TeamDifference based triggers are overwritten.
					if (m_TeamDiff > m_GHost->m_AutoBanTeamDiffMax && m_GHost->m_AutoBanTeamDiffMax > 0) { m_BanOn = false; }
				}
				// if m_AutoBanFirstXLeavers is set check if this leaver has exceeded the max number of leavers to ban. If so turn ban off. Overides all but timer.
				if (m_GHost->m_AutoBanFirstXLeavers > 0 && m_PlayersLeft < m_GHost->m_AutoBanFirstXLeavers) { m_BanOn = true; }
				// If m_AutoBanTimer is set to something other than 0. If time is exceeded then turn off ban. Nothing overides this but auto ban being off.
				if (m_GHost->m_AutoBanTimer > 0) {
					float iTime = (float)(GetTime() - m_GameLoadedTime) / 60;
					if (m_GetMapType == "dota") { if (iTime > 2) { iTime -= 2; } else { iTime = 1; } }
					// If the in game time in mins if over the time set in m_AutoBanTimer then overwrite any triggers that turn on Auto Ban.
					if (iTime > m_GHost->m_AutoBanTimer) { m_BanOn = false; }
				}
			}
			else if (m_GameLoading && m_GHost->m_AutoBanGameLoading) {
				// If game is loading and player is not a admin ban.
				m_BanOn = true;
			}
			else if (m_CountDownStarted && !m_GameLoading && !m_GameLoaded && m_GHost->m_AutoBanCountDown) {
				// If game is loading and player is not a admin ban
				m_BanOn = true;
			}
		}
	}
	// If m_BanOn got turned on for some reason ban.
	if (m_BanOn) {
		string timediff = UTIL_ToString(m_GHost->m_AutoBanGameEndMins);
		// Send info about the leaver
		SendAllChat("[AUTOBAN: " + m_GameName + "] " + player->GetName() + " will be banned if he/she has not left within " + timediff + " mins of game over time.");
		CONSOLE_Print("[AUTOBAN: " + m_GameName + "] Adding " + player->GetName() + " to the temp banlist in case he/she leaves not within " + timediff + " mins of game over time.");
		// Add player to the temp vector
		m_AutoBanTemp.push_back(player->GetName());
	}
	CBaseGame::EventPlayerLeft(player, reason);
}

void CGame :: EventPlayerDeleted( CGamePlayer *player )
{
	CBaseGame :: EventPlayerDeleted( player );

	// record everything we need to know about the player for storing in the database later
	// since we haven't stored the game yet (it's not over yet!) we can't link the gameplayer to the game
	// see the destructor for where these CDBGamePlayers are stored in the database
	// we could have inserted an incomplete record on creation and updated it later but this makes for a cleaner interface

	if( m_GameLoading || m_GameLoaded )
	{
		// todotodo: since we store players that crash during loading it's possible that the stats classes could have no information on them
		// that could result in a DBGamePlayer without a corresponding DBDotAPlayer - just be aware of the possibility

		unsigned char SID = GetSIDFromPID( player->GetPID( ) );
		unsigned char Team = 255;
		unsigned char Colour = 255;

		if( SID < m_Slots.size( ) )
		{
			Team = m_Slots[SID].GetTeam( );
			Colour = m_Slots[SID].GetColour( );
		}

		m_DBGamePlayers.push_back( new CDBGamePlayer( 0, 0, player->GetName( ), player->GetExternalIPString( ), player->GetSpoofed( ) ? 1 : 0, player->GetSpoofedRealm( ), player->GetReserved( ) ? 1 : 0, player->GetFinishedLoading( ) ? player->GetFinishedLoadingTicks( ) - m_StartedLoadingTicks : 0, m_GameTicks / 1000, player->GetLeftReason( ), Team, Colour ) );

		// also keep track of the last player to leave for the !banlast command

		for( vector<CDBBan *> :: iterator i = m_DBBans.begin( ); i != m_DBBans.end( ); ++i )
		{
			if( (*i)->GetName( ) == player->GetName( ) )
				m_DBBanLast = *i;
		}

		// Add to a player leaver counter
		m_PlayersLeft++;
	}
}

bool CGame :: EventPlayerAction( CGamePlayer *player, CIncomingAction *action )
{
	bool success = CBaseGame :: EventPlayerAction( player, action );

	// give the stats class a chance to process the action

	if( success && m_Stats && m_Stats->ProcessAction( action ) && m_GameOverTime == 0 )
	{
		CONSOLE_Print( "[GAME: " + m_GameName + "] gameover timer started (stats class reported game over)" );
		SendEndMessage( );
		m_GameEnded = true;
		m_GameEndedTime = GetTime( );
		m_GameOverTime = GetTime( ); //should we delete this?????
	}
	
	return success;
}

bool CGame :: EventPlayerBotCommand( CGamePlayer *player, string command, string payload )
{
	bool HideCommand = CBaseGame :: EventPlayerBotCommand( player, command, payload );

	// todotodo: don't be lazy

	string User = player->GetName( );
	string Command = command;
	string Payload = payload;

	bool AdminCheck = false;

	for( vector<CBNET *> :: iterator i = m_GHost->m_BNETs.begin( ); i != m_GHost->m_BNETs.end( ); ++i )
	{
		if( (*i)->GetServer( ) == player->GetSpoofedRealm( ) && (*i)->IsAdmin( User ) )
		{
			AdminCheck = true;
			break;
		}
	}

	bool RootAdminCheck = false;

	for( vector<CBNET *> :: iterator i = m_GHost->m_BNETs.begin( ); i != m_GHost->m_BNETs.end( ); ++i )
	{
		if( (*i)->GetServer( ) == player->GetSpoofedRealm( ) && (*i)->IsRootAdmin( User ) )
		{
			RootAdminCheck = true;
			break;
		}
	}

	if (true) 
	{
		/********************/
		/* EVERONE COMMANDS */
		/****************** */

		//
		// !AUTOBAN
		//

		if (Command == "autoban")
		{
			if (!Payload.empty())
			{
				if (AdminCheck || RootAdminCheck) {
					if (Payload == "on") {
						m_AutoBanState = true;
						SendAllChat("Auto ban is ON");
					}
					else if (Payload == "off") {
						m_AutoBanState = false;
						SendAllChat("Auto ban is OFF");
					}
					else {
						SendChat(player->GetPID(), "Invalid argument");
					}
				}
			}
			else
			{
				string reply = "Auto ban is ";
				reply += (m_AutoBanState ? "ON" : "OFF");
				reply += !m_AutoBanState && m_IsLadderGame ? ". This game will be saved to your ladder stats" : string( ); 
				SendChat(player->GetPID(), reply);

			}
		}

		//
		// !CHECK
		//

		else if (Command == "check")
		{
			string reply = string();
			if (!Payload.empty())
			{
				CGamePlayer* LastMatch = NULL;
				uint32_t Matches = GetPlayerFromNamePartial(Payload, &LastMatch);

				if (Matches == 0)
					reply = (m_GHost->m_Language->UnableToCheckPlayerNoMatchesFound(Payload));
				else if (Matches == 1)
				{
					bool LastMatchAdminCheck = false;

					for (vector<CBNET*> ::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); ++i)
					{
						if ((*i)->GetServer() == LastMatch->GetSpoofedRealm() && (*i)->IsAdmin(LastMatch->GetName()))
						{
							LastMatchAdminCheck = true;
							break;
						}
					}

					bool LastMatchRootAdminCheck = false;

					for (vector<CBNET*> ::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); ++i)
					{
						if ((*i)->GetServer() == LastMatch->GetSpoofedRealm() && (*i)->IsRootAdmin(LastMatch->GetName()))
						{
							LastMatchRootAdminCheck = true;
							break;
						}
					}

					reply = (m_GHost->m_Language->CheckedPlayer(LastMatch->GetName(), LastMatch->GetNumPings() > 0 ? UTIL_ToString(LastMatch->GetPing(m_GHost->m_LCPings)) + "ms" : "N/A", m_GHost->m_DBLocal->FromCheck(UTIL_ByteArrayToUInt32(LastMatch->GetExternalIP(), true)), LastMatchAdminCheck || LastMatchRootAdminCheck ? "Yes" : "No", IsOwner(LastMatch->GetName()) ? "Yes" : "No", LastMatch->GetSpoofed() ? "Yes" : "No", LastMatch->GetSpoofedRealm().empty() ? "N/A" : LastMatch->GetSpoofedRealm(), LastMatch->GetReserved() ? "Yes" : "No"));
				}
				else
					reply = (m_GHost->m_Language->UnableToCheckPlayerFoundMoreThanOneMatch(Payload));
			}
			else
				reply = (m_GHost->m_Language->CheckedPlayer(User, player->GetNumPings() > 0 ? UTIL_ToString(player->GetPing(m_GHost->m_LCPings)) + "ms" : "N/A", m_GHost->m_DBLocal->FromCheck(UTIL_ByteArrayToUInt32(player->GetExternalIP(), true)), AdminCheck || RootAdminCheck ? "Yes" : "No", IsOwner(User) ? "Yes" : "No", player->GetSpoofed() ? "Yes" : "No", player->GetSpoofedRealm().empty() ? "N/A" : player->GetSpoofedRealm(), player->GetReserved() ? "Yes" : "No"));
		
			if (!reply.empty()) 
				SendChat(player->GetPID(), reply);
		}

		//
		// !CHECKBAN
		//

		else if (Command == "checkban" && !Payload.empty() && !m_GHost->m_BNETs.empty())
		{
			for (vector<CBNET*> ::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); ++i)
				m_PairedBanChecks.push_back(PairedBanCheck(User, m_GHost->m_DB->ThreadedBanCheck((*i)->GetServer(), Payload, string())));
		}

		//
		// !FROM
		//

		else if (Command == "from" || Command == "f")
		{
			string Froms;

			for (vector<CGamePlayer*> ::iterator i = m_Players.begin(); i != m_Players.end(); ++i)
			{
				// we reverse the byte order on the IP because it's stored in network byte order

				Froms += (*i)->GetNameTerminated();
				Froms += ": (";
				Froms += m_GHost->m_DBLocal->FromCheck(UTIL_ByteArrayToUInt32((*i)->GetExternalIP(), true));
				Froms += ")";

				if (i != m_Players.end() - 1)
					Froms += ", ";

				if ((m_GameLoading || m_GameLoaded) && Froms.size() > 100)
				{
					// cut the text into multiple lines ingame

					if (IsOwner(User) || RootAdminCheck)
						SendAllChat(Froms);
					else
						SendChat(player->GetPID(), Froms);

					Froms.clear();
				}
			}

			if (!Froms.empty())
			{
				if (IsOwner(User) || RootAdminCheck)
					SendAllChat(Froms);
				else
					SendChat(player->GetPID(), Froms);
			}
				
		}

		//
		// !OWNER (set game owner)
		//

		else if (Command == "owner")
		{
			if (RootAdminCheck || IsOwner(User) || !GetPlayerFromName(m_OwnerName, false))
			{
				if (!Payload.empty())
				{
					SendAllChat(m_GHost->m_Language->SettingGameOwnerTo(Payload));
					m_OwnerName = Payload;
				}
				else
				{
					SendAllChat(m_GHost->m_Language->SettingGameOwnerTo(User));
					m_OwnerName = User;
				}
			}
			else
				SendAllChat(m_GHost->m_Language->UnableToSetGameOwner(m_OwnerName));
				}

		//
		// !PING
		//

		else if (Command == "ping" || Command == "p")
		{
			// kick players with ping higher than payload if payload isn't empty
			// we only do this if the game hasn't started since we don't want to kick players from a game in progress

			uint32_t Kicked = 0;
			uint32_t KickPing = 0;

			if (!m_GameLoading && !m_GameLoaded && !Payload.empty())
				KickPing = UTIL_ToUInt32(Payload);

			// copy the m_Players vector so we can sort by descending ping so it's easier to find players with high pings

			vector<CGamePlayer*> SortedPlayers = m_Players;
			sort(SortedPlayers.begin(), SortedPlayers.end(), CGamePlayerSortDescByPing());
			string Pings;

			for (vector<CGamePlayer*> ::iterator i = SortedPlayers.begin(); i != SortedPlayers.end(); ++i)
			{
				Pings += (*i)->GetNameTerminated();
				Pings += ": ";

				if ((*i)->GetNumPings() > 0)
				{
					Pings += UTIL_ToString((*i)->GetPing(m_GHost->m_LCPings));

					if (!m_GameLoading && !m_GameLoaded && !(*i)->GetReserved() && KickPing > 0 && (*i)->GetPing(m_GHost->m_LCPings) > KickPing)
					{
						(*i)->SetDeleteMe(true);
						(*i)->SetLeftReason("was kicked for excessive ping " + UTIL_ToString((*i)->GetPing(m_GHost->m_LCPings)) + " > " + UTIL_ToString(KickPing));
						(*i)->SetLeftCode(PLAYERLEAVE_LOBBY);
						OpenSlot(GetSIDFromPID((*i)->GetPID()), false);
						Kicked++;
					}

					Pings += "ms";
				}
				else
					Pings += "N/A";

				if (i != SortedPlayers.end() - 1)
					Pings += ", ";

				if ((m_GameLoading || m_GameLoaded) && Pings.size() > 100)
				{
					// cut the text into multiple lines ingame

					if (IsOwner(User) || RootAdminCheck)
						SendAllChat(Pings);
					else
						SendChat(player->GetPID(), Pings);
	
					Pings.clear();
				}
			}

			if (!Pings.empty()) {
				if (IsOwner(User) || RootAdminCheck)
					SendAllChat(Pings);
				else
					SendChat(player, Pings);
			}

			if (Kicked > 0)
				SendAllChat(m_GHost->m_Language->KickingPlayersWithPingsGreaterThan(UTIL_ToString(Kicked), UTIL_ToString(KickPing)));

		}


	} //EVERONE COMMANDS **********************



	if( player->GetSpoofed( ) && ( AdminCheck || RootAdminCheck || IsOwner( User ) ) )
	{
		CONSOLE_Print( "[GAME: " + m_GameName + "] admin [" + User + "] sent command [" + Command + "] with payload [" + Payload + "]" );

		if (!m_Locked || RootAdminCheck )
		{
			/******************/
			/* ADMIN COMMANDS */
			/**************** */

			//
			// !ADDBAN
			// !BAN
			//

			if ((Command == "addban" || Command == "ban") && !Payload.empty() && !m_GHost->m_BNETs.empty())
			{
				// extract the victim and the reason
				// e.g. "Varlock leaver after dying" -> victim: "Varlock", reason: "leaver after dying"

				string Victim;
				string Reason;
				stringstream SS;
				SS << Payload;
				SS >> Victim;

				if (!SS.eof())
				{
					getline(SS, Reason);
					string::size_type Start = Reason.find_first_not_of(" ");

					if (Start != string::npos)
						Reason = Reason.substr(Start);
				}

				if (m_GameLoaded)
				{
					string VictimLower = Victim;
					transform(VictimLower.begin(), VictimLower.end(), VictimLower.begin(), (int(*)(int))tolower);
					uint32_t Matches = 0;
					CDBBan* LastMatch = NULL;

					// try to match each player with the passed string (e.g. "Varlock" would be matched with "lock")
					// we use the m_DBBans vector for this in case the player already left and thus isn't in the m_Players vector anymore

					for (vector<CDBBan*> ::iterator i = m_DBBans.begin(); i != m_DBBans.end(); ++i)
					{
						string TestName = (*i)->GetName();
						transform(TestName.begin(), TestName.end(), TestName.begin(), (int(*)(int))tolower);

						if (TestName.find(VictimLower) != string::npos)
						{
							Matches++;
							LastMatch = *i;

							// if the name matches exactly stop any further matching

							if (TestName == VictimLower)
							{
								Matches = 1;
								break;
							}
						}
					}

					if (Matches == 0)
						SendAllChat(m_GHost->m_Language->UnableToBanNoMatchesFound(Victim));
					else if (Matches == 1)
						m_PairedBanAdds.push_back(PairedBanAdd(User, m_GHost->m_DB->ThreadedBanAdd(LastMatch->GetServer(), LastMatch->GetName(), LastMatch->GetIP(), m_GameName, User, Reason)));
					else
						SendAllChat(m_GHost->m_Language->UnableToBanFoundMoreThanOneMatch(Victim));
				}
				else
				{
					CGamePlayer* LastMatch = NULL;
					uint32_t Matches = GetPlayerFromNamePartial(Victim, &LastMatch);

					if (Matches == 0)
						SendAllChat(m_GHost->m_Language->UnableToBanNoMatchesFound(Victim));
					else if (Matches == 1)
						m_PairedBanAdds.push_back(PairedBanAdd(User, m_GHost->m_DB->ThreadedBanAdd(LastMatch->GetJoinedRealm(), LastMatch->GetName(), LastMatch->GetExternalIPString(), m_GameName, User, Reason)));
					else
						SendAllChat(m_GHost->m_Language->UnableToBanFoundMoreThanOneMatch(Victim));
				}
			}

			//
			// !BANLAST
			//

			else if (Command == "banlast" && m_GameLoaded && !m_GHost->m_BNETs.empty() && m_DBBanLast)
				m_PairedBanAdds.push_back(PairedBanAdd(User, m_GHost->m_DB->ThreadedBanAdd(m_DBBanLast->GetServer(), m_DBBanLast->GetName(), m_DBBanLast->GetIP(), m_GameName, User, Payload)));

			//
			// !LOCK
			//

			else if (Command == "lock" && (RootAdminCheck || IsOwner(User)))
			{
				SendAllChat(m_GHost->m_Language->GameLocked());
				m_Locked = true;
			}

			//
			// !MUTE
			//

			else if (Command == "mute")
			{
				CGamePlayer* LastMatch = NULL;
				uint32_t Matches = GetPlayerFromNamePartial(Payload, &LastMatch);

				if (Matches == 0)
					SendAllChat(m_GHost->m_Language->UnableToMuteNoMatchesFound(Payload));
				else if (Matches == 1)
				{
					SendAllChat(m_GHost->m_Language->MutedPlayer(LastMatch->GetName(), User));
					LastMatch->SetMuted(true);
				}
				else
					SendAllChat(m_GHost->m_Language->UnableToMuteFoundMoreThanOneMatch(Payload));
			}

			//
			// !SAY
			//

			else if (Command == "say" && !Payload.empty())
			{
				for (vector<CBNET*> ::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); ++i)
					(*i)->QueueChatCommand(Payload);

				HideCommand = true;
			}

			//
			// !UNLOCK
			//

			else if (Command == "unlock" && (RootAdminCheck || IsOwner(User)))
			{
				SendAllChat(m_GHost->m_Language->GameUnlocked());
				m_Locked = false;
			}

			//
			// !VIRTUALHOST
			//

			else if (Command == "virtualhost" && !Payload.empty() && Payload.size() <= 15 && !m_CountDownStarted)
			{
				DeleteVirtualHost();
				m_VirtualHostName = Payload;
			}

			//
			// !W
			//

			else if (Command == "w" && !Payload.empty())
			{
				// extract the name and the message
				// e.g. "Varlock hello there!" -> name: "Varlock", message: "hello there!"

				string Name;
				string Message;
				string::size_type MessageStart = Payload.find(" ");

				if (MessageStart != string::npos)
				{
					Name = Payload.substr(0, MessageStart);
					Message = Payload.substr(MessageStart + 1);

					for (vector<CBNET*> ::iterator i = m_GHost->m_BNETs.begin(); i != m_GHost->m_BNETs.end(); ++i)
						(*i)->QueueChatCommand(Message, Name, true);
				}

				HideCommand = true;
			}


		} //ADMIN COMMANDS **********************





		if (!m_Locked || (RootAdminCheck || (IsOwner(User)) && !m_IsLadderGame))
		{
			/*****************************************
			* ADMIN & NON-LADDER GAME OWNER COMMANDS *
			******************************************/
			//These are commands that game owners should not be allowed to abuse in ladder/ranked games

			//
			// !DROP
			//

			if (Command == "drop" && m_GameLoaded)
			{
				if (!m_IsLadderGame || AdminCheck)
					StopLaggers("lagged out (dropped by admin)");
			}

			//
			// !END
			//

			else if (Command == "end" && m_GameLoaded)
			{
				CONSOLE_Print("[GAME: " + m_GameName + "] is over (admin ended game)");
				StopPlayers("was disconnected (admin ended game)");
			}

			//
			// !COMP (computer slot)
			//

			else if (Command == "comp" && !Payload.empty() && !m_GameLoading && !m_GameLoaded && !m_SaveGame)
			{
				// extract the slot and the skill
				// e.g. "1 2" -> slot: "1", skill: "2"

				uint32_t Slot;
				uint32_t Skill = 1;
				stringstream SS;
				SS << Payload;
				SS >> Slot;

				if (SS.fail())
					CONSOLE_Print("[GAME: " + m_GameName + "] bad input #1 to comp command");
				else
				{
					if (!SS.eof())
						SS >> Skill;

					if (SS.fail())
						CONSOLE_Print("[GAME: " + m_GameName + "] bad input #2 to comp command");
					else
						ComputerSlot((unsigned char)(Slot - 1), (unsigned char)Skill, true);
				}
			}

			//
			// !COMPCOLOUR (computer colour change)
			//

			else if (Command == "compcolour" && !Payload.empty() && !m_GameLoading && !m_GameLoaded && !m_SaveGame)
			{
				// extract the slot and the colour
				// e.g. "1 2" -> slot: "1", colour: "2"

				uint32_t Slot;
				uint32_t Colour;
				stringstream SS;
				SS << Payload;
				SS >> Slot;

				if (SS.fail())
					CONSOLE_Print("[GAME: " + m_GameName + "] bad input #1 to compcolour command");
				else
				{
					if (SS.eof())
						CONSOLE_Print("[GAME: " + m_GameName + "] missing input #2 to compcolour command");
					else
					{
						SS >> Colour;

						if (SS.fail())
							CONSOLE_Print("[GAME: " + m_GameName + "] bad input #2 to compcolour command");
						else
						{
							unsigned char SID = (unsigned char)(Slot - 1);

							if (!(m_Map->GetMapOptions() & MAPOPT_FIXEDPLAYERSETTINGS) && Colour < MAX_SLOTS && SID < m_Slots.size())
							{
								if (m_Slots[SID].GetSlotStatus() == SLOTSTATUS_OCCUPIED && m_Slots[SID].GetComputer() == 1)
									ColourSlot(SID, Colour);
							}
						}
					}
				}
			}

			//
			// !COMPHANDICAP (computer handicap change)
			//

			else if (Command == "comphandicap" && !Payload.empty() && !m_GameLoading && !m_GameLoaded && !m_SaveGame)
			{
				// extract the slot and the handicap
				// e.g. "1 50" -> slot: "1", handicap: "50"

				uint32_t Slot;
				uint32_t Handicap;
				stringstream SS;
				SS << Payload;
				SS >> Slot;

				if (SS.fail())
					CONSOLE_Print("[GAME: " + m_GameName + "] bad input #1 to comphandicap command");
				else
				{
					if (SS.eof())
						CONSOLE_Print("[GAME: " + m_GameName + "] missing input #2 to comphandicap command");
					else
					{
						SS >> Handicap;

						if (SS.fail())
							CONSOLE_Print("[GAME: " + m_GameName + "] bad input #2 to comphandicap command");
						else
						{
							unsigned char SID = (unsigned char)(Slot - 1);

							if (!(m_Map->GetMapOptions() & MAPOPT_FIXEDPLAYERSETTINGS) && (Handicap == 50 || Handicap == 60 || Handicap == 70 || Handicap == 80 || Handicap == 90 || Handicap == 100) && SID < m_Slots.size())
							{
								if (m_Slots[SID].GetSlotStatus() == SLOTSTATUS_OCCUPIED && m_Slots[SID].GetComputer() == 1)
								{
									m_Slots[SID].SetHandicap((unsigned char)Handicap);
									SendAllSlotInfo();
								}
							}
						}
					}
				}
			}

			//
			// !COMPRACE (computer race change)
			//

			else if (Command == "comprace" && !Payload.empty() && !m_GameLoading && !m_GameLoaded && !m_SaveGame)
			{
				// extract the slot and the race
				// e.g. "1 human" -> slot: "1", race: "human"

				uint32_t Slot;
				string Race;
				stringstream SS;
				SS << Payload;
				SS >> Slot;

				if (SS.fail())
					CONSOLE_Print("[GAME: " + m_GameName + "] bad input #1 to comprace command");
				else
				{
					if (SS.eof())
						CONSOLE_Print("[GAME: " + m_GameName + "] missing input #2 to comprace command");
					else
					{
						getline(SS, Race);
						string::size_type Start = Race.find_first_not_of(" ");

						if (Start != string::npos)
							Race = Race.substr(Start);

						transform(Race.begin(), Race.end(), Race.begin(), (int(*)(int))tolower);
						unsigned char SID = (unsigned char)(Slot - 1);

						if (!(m_Map->GetMapOptions() & MAPOPT_FIXEDPLAYERSETTINGS) && !(m_Map->GetMapFlags() & MAPFLAG_RANDOMRACES) && SID < m_Slots.size())
						{
							if (m_Slots[SID].GetSlotStatus() == SLOTSTATUS_OCCUPIED && m_Slots[SID].GetComputer() == 1)
							{
								if (Race == "human")
								{
									m_Slots[SID].SetRace(SLOTRACE_HUMAN | SLOTRACE_SELECTABLE);
									SendAllSlotInfo();
								}
								else if (Race == "orc")
								{
									m_Slots[SID].SetRace(SLOTRACE_ORC | SLOTRACE_SELECTABLE);
									SendAllSlotInfo();
								}
								else if (Race == "night elf")
								{
									m_Slots[SID].SetRace(SLOTRACE_NIGHTELF | SLOTRACE_SELECTABLE);
									SendAllSlotInfo();
								}
								else if (Race == "undead")
								{
									m_Slots[SID].SetRace(SLOTRACE_UNDEAD | SLOTRACE_SELECTABLE);
									SendAllSlotInfo();
								}
								else if (Race == "random")
								{
									m_Slots[SID].SetRace(SLOTRACE_RANDOM | SLOTRACE_SELECTABLE);
									SendAllSlotInfo();
								}
								else
									CONSOLE_Print("[GAME: " + m_GameName + "] unknown race [" + Race + "] sent to comprace command");
							}
						}
					}
				}
			}

			//
			// !COMPTEAM (computer team change)
			//

			else if (Command == "compteam" && !Payload.empty() && !m_GameLoading && !m_GameLoaded && !m_SaveGame)
			{
				// extract the slot and the team
				// e.g. "1 2" -> slot: "1", team: "2"

				uint32_t Slot;
				uint32_t Team;
				stringstream SS;
				SS << Payload;
				SS >> Slot;

				if (SS.fail())
					CONSOLE_Print("[GAME: " + m_GameName + "] bad input #1 to compteam command");
				else
				{
					if (SS.eof())
						CONSOLE_Print("[GAME: " + m_GameName + "] missing input #2 to compteam command");
					else
					{
						SS >> Team;

						if (SS.fail())
							CONSOLE_Print("[GAME: " + m_GameName + "] bad input #2 to compteam command");
						else
						{
							unsigned char SID = (unsigned char)(Slot - 1);

							if (!(m_Map->GetMapOptions() & MAPOPT_FIXEDPLAYERSETTINGS) && Team < MAX_SLOTS && SID < m_Slots.size())
							{
								if (m_Slots[SID].GetSlotStatus() == SLOTSTATUS_OCCUPIED && m_Slots[SID].GetComputer() == 1)
								{
									m_Slots[SID].SetTeam((unsigned char)(Team - 1));
									SendAllSlotInfo();
								}
							}
						}
					}
				}
			}

			//
			// !FAKEPLAYER
			//

			else if (Command == "fakeplayer" && !m_CountDownStarted)
			{
				if (m_FakePlayerPID == 255)
					CreateFakePlayer();
				else
					DeleteFakePlayer();
			}

			//
			// !FPPAUSE
			//

			else if (Command == "fppause" && m_FakePlayerPID != 255 && m_GameLoaded)
			{
				BYTEARRAY CRC;
				BYTEARRAY Action;
				Action.push_back(1);
				m_Actions.push(new CIncomingAction(m_FakePlayerPID, CRC, Action));
			}

			//
			// !FPRESUME
			//

			else if (Command == "fpresume" && m_FakePlayerPID != 255 && m_GameLoaded)
			{
				BYTEARRAY CRC;
				BYTEARRAY Action;
				Action.push_back(2);
				m_Actions.push(new CIncomingAction(m_FakePlayerPID, CRC, Action));
			}

			//
			// !UNHOST
			//

			else if (Command == "unhost" && !m_CountDownStarted)
				m_Exiting = true;

			//
			// !UNMUTE
			//

			else if (Command == "unmute")
			{
				CGamePlayer* LastMatch = NULL;
				uint32_t Matches = GetPlayerFromNamePartial(Payload, &LastMatch);

				if (Matches == 0)
					SendAllChat(m_GHost->m_Language->UnableToMuteNoMatchesFound(Payload));
				else if (Matches == 1)
				{
					SendAllChat(m_GHost->m_Language->UnmutedPlayer(LastMatch->GetName(), User));
					LastMatch->SetMuted(false);
				}
				else
					SendAllChat(m_GHost->m_Language->UnableToMuteFoundMoreThanOneMatch(Payload));
			}




		} //ADMIN & NON-LADDER GAME OWNER COMMANDS **********************


		if( !m_Locked || RootAdminCheck || IsOwner( User ) )
		{
			/******************************
			* ADMIN & GAME OWNER COMMANDS *
			*******************************/

			//
			// !ABORT (abort countdown)
			// !A
			//

			// we use "!a" as an alias for abort because you don't have much time to abort the countdown so it's useful for the abort command to be easy to type

			if( ( Command == "abort" || Command == "a" ) && m_CountDownStarted && !m_GameLoading && !m_GameLoaded )
			{
				SendAllChat( m_GHost->m_Language->CountDownAborted( ) );
				m_CountDownStarted = false;
			}

			//
			// !ANNOUNCE
			//

			else if( Command == "announce" && !m_CountDownStarted )
			{
				if( Payload.empty( ) || Payload == "off" )
				{
					SendAllChat( m_GHost->m_Language->AnnounceMessageDisabled( ) );
					SetAnnounce( 0, string( ) );
				}
				else
				{
					// extract the interval and the message
					// e.g. "30 hello everyone" -> interval: "30", message: "hello everyone"

					uint32_t Interval;
					string Message;
					stringstream SS;
					SS << Payload;
					SS >> Interval;

					if( SS.fail( ) || Interval == 0 )
						CONSOLE_Print( "[GAME: " + m_GameName + "] bad input #1 to announce command" );
					else
					{
						if( SS.eof( ) )
							CONSOLE_Print( "[GAME: " + m_GameName + "] missing input #2 to announce command" );
						else
						{
							getline( SS, Message );
							string :: size_type Start = Message.find_first_not_of( " " );

							if( Start != string :: npos )
								Message = Message.substr( Start );

							SendAllChat( m_GHost->m_Language->AnnounceMessageEnabled( ) );
							SetAnnounce( Interval, Message );
						}
					}
				}
			}

			//
			// !AUTOSAVE
			//

			else if( Command == "autosave" )
			{
				if( Payload == "on" )
				{
					SendAllChat( m_GHost->m_Language->AutoSaveEnabled( ) );
					m_AutoSave = true;
				}
				else if( Payload == "off" )
				{
					SendAllChat( m_GHost->m_Language->AutoSaveDisabled( ) );
					m_AutoSave = false;
				}
			}

			//
			// !AUTOSTART
			//

			else if( Command == "autostart" && !m_CountDownStarted )
			{
				if( Payload.empty( ) || Payload == "off" )
				{
					SendAllChat( m_GHost->m_Language->AutoStartDisabled( ) );
					m_AutoStartPlayers = 0;
				}
				else
				{
					uint32_t AutoStartPlayers = UTIL_ToUInt32( Payload );

					if( AutoStartPlayers != 0 )
					{
						SendAllChat( m_GHost->m_Language->AutoStartEnabled( UTIL_ToString( AutoStartPlayers ) ) );
						m_AutoStartPlayers = AutoStartPlayers;
					}
				}
			}
			
			//
			// !CLEARHCL
			//

			else if( Command == "clearhcl" && !m_CountDownStarted )
			{
				m_HCLCommandString.clear( );
				SendAllChat( m_GHost->m_Language->ClearingHCL( ) );
			}

			//
			// !CLOSE (close slot)
			//

			else if( Command == "close" && !Payload.empty( ) && !m_GameLoading && !m_GameLoaded )
			{
				// close as many slots as specified, e.g. "5 10" closes slots 5 and 10

				stringstream SS;
				SS << Payload;

				while( !SS.eof( ) )
				{
					uint32_t SID;
					SS >> SID;

					if( SS.fail( ) )
					{
						CONSOLE_Print( "[GAME: " + m_GameName + "] bad input to close command" );
						break;
					}
					else
						CloseSlot( (unsigned char)( SID - 1 ), true );
				}
			}

			//
			// !CLOSEALL
			//

			else if( Command == "closeall" && !m_GameLoading && !m_GameLoaded )
				CloseAllSlots( );

			//
			// !DBSTATUS
			//

			else if( Command == "dbstatus" )
				SendAllChat( m_GHost->m_DB->GetStatus( ) );

			//
			// !DOWNLOAD
			// !DL
			//

			else if( ( Command == "download" || Command == "dl" ) && !Payload.empty( ) && !m_GameLoading && !m_GameLoaded )
			{
				CGamePlayer *LastMatch = NULL;
				uint32_t Matches = GetPlayerFromNamePartial( Payload, &LastMatch );

				if( Matches == 0 )
					SendAllChat( m_GHost->m_Language->UnableToStartDownloadNoMatchesFound( Payload ) );
				else if( Matches == 1 )
				{
					if( !LastMatch->GetDownloadStarted( ) && !LastMatch->GetDownloadFinished( ) )
					{
						unsigned char SID = GetSIDFromPID( LastMatch->GetPID( ) );

						if( SID < m_Slots.size( ) && m_Slots[SID].GetDownloadStatus( ) != 100 )
						{
							// inform the client that we are willing to send the map

							CONSOLE_Print( "[GAME: " + m_GameName + "] map download started for player [" + LastMatch->GetName( ) + "]" );
							Send( LastMatch, m_Protocol->SEND_W3GS_STARTDOWNLOAD( GetHostPID( ) ) );
							LastMatch->SetDownloadAllowed( true );
							LastMatch->SetDownloadStarted( true );
							LastMatch->SetStartedDownloadingTicks( GetTicks( ) );
						}
					}
				}
				else
					SendAllChat( m_GHost->m_Language->UnableToStartDownloadFoundMoreThanOneMatch( Payload ) );
			}

			//
			// !HCL
			//

			else if( Command == "hcl" && !m_CountDownStarted )
			{
				if( !Payload.empty( ) )
				{
					if( Payload.size( ) <= m_Slots.size( ) )
					{
						string HCLChars = "abcdefghijklmnopqrstuvwxyz0123456789 -=,.";

						if( Payload.find_first_not_of( HCLChars ) == string :: npos )
						{
							m_HCLCommandString = Payload;
							SendAllChat( m_GHost->m_Language->SettingHCL( m_HCLCommandString ) );
						}
						else
							SendAllChat( m_GHost->m_Language->UnableToSetHCLInvalid( ) );
					}
					else
						SendAllChat( m_GHost->m_Language->UnableToSetHCLTooLong( ) );
				}
				else
					SendAllChat( m_GHost->m_Language->TheHCLIs( m_HCLCommandString ) );
			}

			//
			// !HOLD (hold a slot for someone)
			//

			else if( Command == "hold" && !Payload.empty( ) && !m_GameLoading && !m_GameLoaded )
			{
				// hold as many players as specified, e.g. "Varlock Kilranin" holds players "Varlock" and "Kilranin"

				stringstream SS;
				SS << Payload;

				while( !SS.eof( ) )
				{
					string HoldName;
					SS >> HoldName;

					if( SS.fail( ) )
					{
						CONSOLE_Print( "[GAME: " + m_GameName + "] bad input to hold command" );
						break;
					}
					else
					{
						SendAllChat( m_GHost->m_Language->AddedPlayerToTheHoldList( HoldName ) );
						AddToReserved( HoldName );
					}
				}
			}

			//
			// !KICK (kick a player)
			//

			else if( Command == "kick" && !Payload.empty( ) )
			{
				CGamePlayer *LastMatch = NULL;
				uint32_t Matches = GetPlayerFromNamePartial( Payload, &LastMatch );

				if( Matches == 0 )
					SendAllChat( m_GHost->m_Language->UnableToKickNoMatchesFound( Payload ) );
				else if( Matches == 1 )
				{
					LastMatch->SetDeleteMe( true );
					LastMatch->SetLeftReason( m_GHost->m_Language->WasKickedByPlayer( User ) );

					if( !m_GameLoading && !m_GameLoaded )
						LastMatch->SetLeftCode( PLAYERLEAVE_LOBBY );
					else
						LastMatch->SetLeftCode( PLAYERLEAVE_LOST );

					if( !m_GameLoading && !m_GameLoaded )
						OpenSlot( GetSIDFromPID( LastMatch->GetPID( ) ), false );
				}
				else
					SendAllChat( m_GHost->m_Language->UnableToKickFoundMoreThanOneMatch( Payload ) );
			}

			//
			// !LATENCY (set game latency)
			//

			else if( Command == "latency" )
			{
				if( Payload.empty( ) )
					SendAllChat( m_GHost->m_Language->LatencyIs( UTIL_ToString( m_Latency ) ) );
				else
				{
					m_Latency = UTIL_ToUInt32( Payload );

					if( m_Latency <= 20 )
					{
						m_Latency = 20;
						SendAllChat( m_GHost->m_Language->SettingLatencyToMinimum( "20" ) );
					}
					else if( m_Latency >= 500 )
					{
						m_Latency = 500;
						SendAllChat( m_GHost->m_Language->SettingLatencyToMaximum( "500" ) );
					}
					else
						SendAllChat( m_GHost->m_Language->SettingLatencyTo( UTIL_ToString( m_Latency ) ) );
				}
			}

			//
			// !MESSAGES
			//

			else if( Command == "messages" )
			{
				if( Payload == "on" )
				{
					SendAllChat( m_GHost->m_Language->LocalAdminMessagesEnabled( ) );
					m_LocalAdminMessages = true;
				}
				else if( Payload == "off" )
				{
					SendAllChat( m_GHost->m_Language->LocalAdminMessagesDisabled( ) );
					m_LocalAdminMessages = false;
				}
			}

			//
			// !MUTEALL
			//

			else if( Command == "muteall" && m_GameLoaded )
			{
				SendAllChat( m_GHost->m_Language->GlobalChatMuted( ) );
				m_MuteAll = true;
			}

			//
			// !OPEN (open slot)
			//

			else if( Command == "open" && !Payload.empty( ) && !m_GameLoading && !m_GameLoaded )
			{
				// open as many slots as specified, e.g. "5 10" opens slots 5 and 10

				stringstream SS;
				SS << Payload;

				while( !SS.eof( ) )
				{
					uint32_t SID;
					SS >> SID;

					if( SS.fail( ) )
					{
						CONSOLE_Print( "[GAME: " + m_GameName + "] bad input to open command" );
						break;
					}
					else
						OpenSlot( (unsigned char)( SID - 1 ), true );
				}
			}

			//
			// !OPENALL
			//

			else if( Command == "openall" && !m_GameLoading && !m_GameLoaded )
				OpenAllSlots( );

			//
			// !PRIV (rehost as private game)
			//

			else if( Command == "priv" && !Payload.empty( ) && !m_CountDownStarted && !m_SaveGame )
			{
				if( Payload.length() < 31 )
				{
					CONSOLE_Print( "[GAME: " + m_GameName + "] trying to rehost as private game [" + Payload + "]" );
					SendAllChat( m_GHost->m_Language->TryingToRehostAsPrivateGame( Payload ) );
					m_GameState = GAME_PRIVATE;
					m_LastGameName = m_GameName;
					m_GameName = Payload;
					m_HostCounter = m_GHost->m_HostCounter++;
					m_RefreshError = false;
					m_RefreshRehosted = true;

					for( vector<CBNET *> :: iterator i = m_GHost->m_BNETs.begin( ); i != m_GHost->m_BNETs.end( ); ++i )
					{
						// unqueue any existing game refreshes because we're going to assume the next successful game refresh indicates that the rehost worked
						// this ignores the fact that it's possible a game refresh was just sent and no response has been received yet
						// we assume this won't happen very often since the only downside is a potential false positive

						(*i)->UnqueueGameRefreshes( );
						(*i)->QueueGameUncreate( );
						(*i)->QueueEnterChat( );

						// we need to send the game creation message now because private games are not refreshed

						(*i)->QueueGameCreate( m_GameState, m_GameName, string( ), m_Map, NULL, m_HostCounter );

						if( (*i)->GetPasswordHashType( ) != "pvpgn" )
							(*i)->QueueEnterChat( );
					}

					m_CreationTime = GetTime( );
					m_LastRefreshTime = GetTime( );
				}
				else
					SendAllChat( m_GHost->m_Language->UnableToCreateGameNameTooLong( Payload ) );
			}

			//
			// !PUB (rehost as public game)
			//

			else if( Command == "pub" && !Payload.empty( ) && !m_CountDownStarted && !m_SaveGame )
			{
				if( Payload.length() < 31 )
				{
					CONSOLE_Print( "[GAME: " + m_GameName + "] trying to rehost as public game [" + Payload + "]" );
					SendAllChat( m_GHost->m_Language->TryingToRehostAsPublicGame( Payload ) );
					m_GameState = GAME_PUBLIC;
					m_LastGameName = m_GameName;
					m_GameName = Payload;
					m_HostCounter = m_GHost->m_HostCounter++;
					m_RefreshError = false;
					m_RefreshRehosted = true;

					for( vector<CBNET *> :: iterator i = m_GHost->m_BNETs.begin( ); i != m_GHost->m_BNETs.end( ); ++i )
					{
						// unqueue any existing game refreshes because we're going to assume the next successful game refresh indicates that the rehost worked
						// this ignores the fact that it's possible a game refresh was just sent and no response has been received yet
						// we assume this won't happen very often since the only downside is a potential false positive

						(*i)->UnqueueGameRefreshes( );
						(*i)->QueueGameUncreate( );
						(*i)->QueueEnterChat( );

						// the game creation message will be sent on the next refresh
					}

					m_CreationTime = GetTime( );
					m_LastRefreshTime = GetTime( );
				}
				else
					SendAllChat( m_GHost->m_Language->UnableToCreateGameNameTooLong( Payload ) );
			}
			//
			// !REFRESH (turn on or off refresh messages)
			//

			else if( Command == "refresh" && !m_CountDownStarted )
			{
				if( Payload == "on" )
				{
					SendAllChat( m_GHost->m_Language->RefreshMessagesEnabled( ) );
					m_RefreshMessages = true;
				}
				else if( Payload == "off" )
				{
					SendAllChat( m_GHost->m_Language->RefreshMessagesDisabled( ) );
					m_RefreshMessages = false;
				}
			}

			//
			// !SENDLAN
			//

			else if( Command == "sendlan" && !Payload.empty( ) && !m_CountDownStarted )
			{
				// extract the ip and the port
				// e.g. "1.2.3.4 6112" -> ip: "1.2.3.4", port: "6112"

				string IP;
				uint32_t Port = 6112;
				stringstream SS;
				SS << Payload;
				SS >> IP;

				if( !SS.eof( ) )
					SS >> Port;

				if( SS.fail( ) )
					CONSOLE_Print( "[GAME: " + m_GameName + "] bad inputs to sendlan command" );
				else
				{
					// construct a fixed host counter which will be used to identify players from this "realm" (i.e. LAN)
					// the fixed host counter's 4 most significant bits will contain a 4 bit ID (0-15)
					// the rest of the fixed host counter will contain the 28 least significant bits of the actual host counter
					// since we're destroying 4 bits of information here the actual host counter should not be greater than 2^28 which is a reasonable assumption
					// when a player joins a game we can obtain the ID from the received host counter
					// note: LAN broadcasts use an ID of 0, battle.net refreshes use an ID of 1-10, the rest are unused

					uint32_t FixedHostCounter = m_HostCounter & 0x0FFFFFFF;

					// we send MAX_SLOTS for SlotsTotal because this determines how many PID's Warcraft 3 allocates
					// we need to make sure Warcraft 3 allocates at least SlotsTotal + 1 but at most MAX_SLOTS PID's
					// this is because we need an extra PID for the virtual host player (but we always delete the virtual host player when the MAX_SLOTSth person joins)
					// however, we can't send 13 for SlotsTotal because this causes Warcraft 3 to crash when sharing control of units
					// nor can we send SlotsTotal because then Warcraft 3 crashes when playing maps with less than MAX_SLOTS PID's (because of the virtual host player taking an extra PID)
					// we also send MAX_SLOTS for SlotsOpen because Warcraft 3 assumes there's always at least one player in the game (the host)
					// so if we try to send accurate numbers it'll always be off by one and results in Warcraft 3 assuming the game is full when it still needs one more player
					// the easiest solution is to simply send MAX_SLOTS for both so the game will always show up as (1/MAX_SLOTS) players

					if( m_SaveGame )
					{
						// note: the PrivateGame flag is not set when broadcasting to LAN (as you might expect)

						uint32_t MapGameType = MAPGAMETYPE_SAVEDGAME;
						BYTEARRAY MapWidth;
						MapWidth.push_back( 0 );
						MapWidth.push_back( 0 );
						BYTEARRAY MapHeight;
						MapHeight.push_back( 0 );
						MapHeight.push_back( 0 );
						m_GHost->m_UDPSocket->SendTo( IP, Port, m_Protocol->SEND_W3GS_GAMEINFO( m_GHost->m_TFT, m_GHost->m_LANWar3Version, UTIL_CreateByteArray( MapGameType, false ), m_Map->GetMapGameFlags( ), MapWidth, MapHeight, m_GameName, "Varlock", GetTime( ) - m_CreationTime, "Save\\Multiplayer\\" + m_SaveGame->GetFileNameNoPath( ), m_SaveGame->GetMagicNumber( ), MAX_SLOTS, MAX_SLOTS, m_HostPort, FixedHostCounter, m_EntryKey ) );
					}
					else
					{
						// note: the PrivateGame flag is not set when broadcasting to LAN (as you might expect)
						// note: we do not use m_Map->GetMapGameType because none of the filters are set when broadcasting to LAN (also as you might expect)

						uint32_t MapGameType = MAPGAMETYPE_UNKNOWN0;
						m_GHost->m_UDPSocket->SendTo( IP, Port, m_Protocol->SEND_W3GS_GAMEINFO( m_GHost->m_TFT, m_GHost->m_LANWar3Version, UTIL_CreateByteArray( MapGameType, false ), m_Map->GetMapGameFlags( ), m_Map->GetMapWidth( ), m_Map->GetMapHeight( ), m_GameName, "Varlock", GetTime( ) - m_CreationTime, m_Map->GetMapPath( ), m_Map->GetMapCRC( ), MAX_SLOTS, MAX_SLOTS, m_HostPort, FixedHostCounter, m_EntryKey ) );
					}
				}
			}

			//
			// !SP
			//

			else if( Command == "sp" && !m_CountDownStarted )
			{
				SendAllChat( m_GHost->m_Language->ShufflingPlayers( ) );
				ShuffleSlots( );
			}

			//
			// !START
			//

			else if( Command == "start" && !m_CountDownStarted )
			{
				// if the player sent "!start force" skip the checks and start the countdown
				// otherwise check that the game is ready to start

				if( Payload == "force" )
					StartCountDown( true );
				else
				{
					if( GetTicks( ) - m_LastPlayerLeaveTicks >= 2000 )
						StartCountDown( false );
					else
						SendAllChat( m_GHost->m_Language->CountDownAbortedSomeoneLeftRecently( ) );
				}
			}

			//
			// !SWAP (swap slots)
			//

			else if( Command == "swap" && !Payload.empty( ) && !m_GameLoading && !m_GameLoaded )
			{
				uint32_t SID1;
				uint32_t SID2;
				stringstream SS;
				SS << Payload;
				SS >> SID1;

				if( SS.fail( ) )
					CONSOLE_Print( "[GAME: " + m_GameName + "] bad input #1 to swap command" );
				else
				{
					if( SS.eof( ) )
						CONSOLE_Print( "[GAME: " + m_GameName + "] missing input #2 to swap command" );
					else
					{
						SS >> SID2;

						if( SS.fail( ) )
							CONSOLE_Print( "[GAME: " + m_GameName + "] bad input #2 to swap command" );
						else
							SwapSlots( (unsigned char)( SID1 - 1 ), (unsigned char)( SID2 - 1 ) );
					}
				}
			}

			//
			// !SYNCLIMIT
			//

			else if( Command == "synclimit" || Command == "sync")
			{
				if( Payload.empty( ) )
					SendAllChat( m_GHost->m_Language->SyncLimitIs( UTIL_ToString( m_SyncLimit ) ) );
				else
				{
					m_SyncLimit = UTIL_ToUInt32( Payload );

					if( m_SyncLimit <= 10 )
					{
						m_SyncLimit = 10;
						SendAllChat( m_GHost->m_Language->SettingSyncLimitToMinimum( "10" ) );
					}
					else if( m_SyncLimit >= 10000 )
					{
						m_SyncLimit = 10000;
						SendAllChat( m_GHost->m_Language->SettingSyncLimitToMaximum( "10000" ) );
					}
					else
						SendAllChat( m_GHost->m_Language->SettingSyncLimitTo( UTIL_ToString( m_SyncLimit ) ) );
				}
			}

			//
			// !UNMUTEALL
			//

			else if( Command == "unmuteall" && m_GameLoaded )
			{
				SendAllChat( m_GHost->m_Language->GlobalChatUnmuted( ) );
				m_MuteAll = false;
			}

			//
			// !VOTECANCEL
			//

			else if( Command == "votecancel" && !m_KickVotePlayer.empty( ) )
			{
				SendAllChat( m_GHost->m_Language->VoteKickCancelled( m_KickVotePlayer ) );
				m_KickVotePlayer.clear( );
				m_StartedKickVoteTime = 0;
			}

		}
		else
		{
			CONSOLE_Print( "[GAME: " + m_GameName + "] admin command ignored, the game is locked" );
			SendChat( player, m_GHost->m_Language->TheGameIsLocked( ) );
		}
	}
	else
	{
		if( !player->GetSpoofed( ) )
			CONSOLE_Print( "[GAME: " + m_GameName + "] non-spoofchecked user [" + User + "] sent command [" + Command + "] with payload [" + Payload + "]" );
		else
			CONSOLE_Print( "[GAME: " + m_GameName + "] non-admin [" + User + "] sent command [" + Command + "] with payload [" + Payload + "]" );
	}

	/*********************
	* NON ADMIN COMMANDS *
	*********************/

	//
	// !CHECKME
	//

	if( Command == "checkme" )
		SendChat( player, m_GHost->m_Language->CheckedPlayer( User, player->GetNumPings( ) > 0 ? UTIL_ToString( player->GetPing( m_GHost->m_LCPings ) ) + "ms" : "N/A", m_GHost->m_DBLocal->FromCheck( UTIL_ByteArrayToUInt32( player->GetExternalIP( ), true ) ), AdminCheck || RootAdminCheck ? "Yes" : "No", IsOwner( User ) ? "Yes" : "No", player->GetSpoofed( ) ? "Yes" : "No", player->GetSpoofedRealm( ).empty( ) ? "N/A" : player->GetSpoofedRealm( ), player->GetReserved( ) ? "Yes" : "No" ) );

	//
	// !RALL
	//

	if (Command == "rall" || Command == "ratingall")
	{
		/*
		string reply  = string( );
		BYTEARRAY IDs = m_GHost->m_CurrentGame->GetPIDs();
		for (BYTEARRAY::iterator i = IDs.begin(); i != IDs.end(); i++)
		{
			CGamePlayer* plyr = m_GHost->m_CurrentGame->GetPlayerFromPID(*i);
			CDBDotAPlayerSummaryNew = plyr->GetDotASummary
			reply += plyr->GetName() + " ";

		}
		*/

		string reply = string();
		uint32_t Kicked = 0;
		int baseRating = 1500;
		int maxAllowedRating = +9999;
		int minAllowedRating = -9999;

		vector<CGamePlayer*> SortedPlayers = m_Players;
		sort(SortedPlayers.begin(), SortedPlayers.end(), CGamePlayerSortDescByRating());

		for (vector<CGamePlayer*> ::iterator i = SortedPlayers.begin(); i != SortedPlayers.end(); i++)
		{
			CDBDotAPlayerSummaryNew* dotaPlayerSummary = (*i)->GetDotASummary();
			//int adrs = dotaPlayerSummary;
			reply += (*i)->GetNameTerminated();
			reply += "(";

			uint32_t playerRating = (*i)->GetDotARating();
			if (dotaPlayerSummary)
			{
				playerRating = dotaPlayerSummary->GetRating();
				reply += UTIL_ToString(playerRating) + ") ";
			}
			else
			{
				playerRating = 1500;
				reply += UTIL_ToString(playerRating) + "!) ";
			}

			if (playerRating > maxAllowedRating)
			{
				(*i)->SetDeleteMe(true);
				(*i)->SetLeftReason("was kicked for having too high PSR of " + UTIL_ToString(playerRating));
				(*i)->SetLeftCode(PLAYERLEAVE_LOBBY);
				OpenSlot(GetSIDFromPID((*i)->GetPID()), false);
				Kicked++;
			}
			/*
			if ( playerRating < minAllowedRating )
			{
				(*i)->SetDeleteMe( true );
				(*i)->SetLeftReason( "was kicked for having too low PSR of " + UTIL_ToString( playerRating ) );
				(*i)->SetLeftCode( PLAYERLEAVE_LOBBY );
				OpenSlot( GetSIDFromPID( (*i)->GetPID( ) ), false );
				Kicked++;
			}
			*/
		}

		if (IsOwner(User) || RootAdminCheck)
			SendAllChat(reply);
		else
			SendChat(player->GetPID(), reply);


	}
	//
	// !STATS
	//

	else if( Command == "stats" && GetTime( ) - player->GetStatsSentTime( ) >= 5 )
	{
		string StatsUser = User;

		if( !Payload.empty( ) )
			StatsUser = Payload;

		if( player->GetSpoofed( ) && ( AdminCheck || RootAdminCheck || IsOwner( User ) ) )
			m_PairedGPSChecks.push_back( PairedGPSCheck( string( ), m_GHost->m_DB->ThreadedGamePlayerSummaryCheck( StatsUser ) ) );
		else
			m_PairedGPSChecks.push_back( PairedGPSCheck( User, m_GHost->m_DB->ThreadedGamePlayerSummaryCheck( StatsUser ) ) );

		player->SetStatsSentTime( GetTime( ) );
	}

	//
	// !SD (New)
	//

	if (Command == "sd" && GetTime() >= player->GetStatsDotASentTime() + 1)
	{
		string StatsUser = User;
		string GameState = string();

		if (!Payload.empty())
		{
			StatsUser = Payload;

			CGamePlayer* LastMatch = NULL;

			uint32_t Matches = GetPlayerFromNamePartial(Payload, &LastMatch);
			if (Matches == 1)
				StatsUser = LastMatch->GetName();
		}

		bool nonadmin = !((player->GetSpoofed() && AdminCheck) || RootAdminCheck || IsOwner(User));

		string m_ScoreMinGames = "1";
		if (!nonadmin)
			m_PairedDPSChecksNew.push_back(PairedDPSCheckNew("%", m_GHost->m_DB->ThreadedDotAPlayerSummaryCheckNew(string(), StatsUser, m_ScoreMinGames, GameState)));
		else
			m_PairedDPSChecksNew.push_back(PairedDPSCheckNew("%" + User, m_GHost->m_DB->ThreadedDotAPlayerSummaryCheckNew(string(), StatsUser, m_ScoreMinGames, GameState)));

		player->SetStatsDotASentTime(GetTime());
	}

	//
	// !STATSDOTA
	// !SDOLD
	//

	else if( (Command == "statsdota" || Command == "sdold") && GetTime( ) - player->GetStatsDotASentTime( ) >= 5 )
	{
		string StatsUser = User;

		if( !Payload.empty( ) )
			StatsUser = Payload;

		if( player->GetSpoofed( ) && ( AdminCheck || RootAdminCheck || IsOwner( User ) ) )
			m_PairedDPSChecks.push_back( PairedDPSCheck( string( ), m_GHost->m_DB->ThreadedDotAPlayerSummaryCheck( StatsUser ) ) );
		else
			m_PairedDPSChecks.push_back( PairedDPSCheck( User, m_GHost->m_DB->ThreadedDotAPlayerSummaryCheck( StatsUser ) ) );

		player->SetStatsDotASentTime( GetTime( ) );
	}

	//
	// !VERSION
	//

	else if( Command == "version" )
	{
		if( player->GetSpoofed( ) && ( AdminCheck || RootAdminCheck || IsOwner( User ) ) )
			SendChat( player, m_GHost->m_Language->VersionAdmin( m_GHost->m_Version ) );
		else
			SendChat( player, m_GHost->m_Language->VersionNotAdmin( m_GHost->m_Version ) );
	}

	//
	// !VOTEKICK
	//

	else if( Command == "votekick" && m_GHost->m_VoteKickAllowed && !Payload.empty( ) )
	{
		if( !m_KickVotePlayer.empty( ) )
			SendChat( player, m_GHost->m_Language->UnableToVoteKickAlreadyInProgress( ) );
		else if( m_Players.size( ) == 2 )
			SendChat( player, m_GHost->m_Language->UnableToVoteKickNotEnoughPlayers( ) );
		else
		{
			CGamePlayer *LastMatch = NULL;
			uint32_t Matches = GetPlayerFromNamePartial( Payload, &LastMatch );

			if( Matches == 0 )
				SendChat( player, m_GHost->m_Language->UnableToVoteKickNoMatchesFound( Payload ) );
			else if( Matches == 1 )
			{
				if( LastMatch->GetReserved( ) )
					SendChat( player, m_GHost->m_Language->UnableToVoteKickPlayerIsReserved( LastMatch->GetName( ) ) );
				else
				{
					m_KickVotePlayer = LastMatch->GetName( );
					m_StartedKickVoteTime = GetTime( );

					for( vector<CGamePlayer *> :: iterator i = m_Players.begin( ); i != m_Players.end( ); ++i )
						(*i)->SetKickVote( false );

					player->SetKickVote( true );
					CONSOLE_Print( "[GAME: " + m_GameName + "] votekick against player [" + m_KickVotePlayer + "] started by player [" + User + "]" );
					SendAllChat( m_GHost->m_Language->StartedVoteKick( LastMatch->GetName( ), User, UTIL_ToString( (uint32_t)ceil( ( GetNumHumanPlayers( ) - 1 ) * (float)m_GHost->m_VoteKickPercentage / 100 ) - 1 ) ) );
					SendAllChat( m_GHost->m_Language->TypeYesToVote( string( 1, m_GHost->m_CommandTrigger ) ) );
				}
			}
			else
				SendChat( player, m_GHost->m_Language->UnableToVoteKickFoundMoreThanOneMatch( Payload ) );
		}
	}

	//
	// !YES
	//

	else if( Command == "yes" && !m_KickVotePlayer.empty( ) && player->GetName( ) != m_KickVotePlayer && !player->GetKickVote( ) )
	{
		player->SetKickVote( true );
		uint32_t VotesNeeded = (uint32_t)ceil( ( GetNumHumanPlayers( ) - 1 ) * (float)m_GHost->m_VoteKickPercentage / 100 );
		uint32_t Votes = 0;

		for( vector<CGamePlayer *> :: iterator i = m_Players.begin( ); i != m_Players.end( ); ++i )
		{
			if( (*i)->GetKickVote( ) )
				++Votes;
		}

		if( Votes >= VotesNeeded )
		{
			CGamePlayer *Victim = GetPlayerFromName( m_KickVotePlayer, true );

			if( Victim )
			{
				Victim->SetDeleteMe( true );
				Victim->SetLeftReason( m_GHost->m_Language->WasKickedByVote( ) );

				if( !m_GameLoading && !m_GameLoaded )
					Victim->SetLeftCode( PLAYERLEAVE_LOBBY );
				else
					Victim->SetLeftCode( PLAYERLEAVE_LOST );

				if( !m_GameLoading && !m_GameLoaded )
					OpenSlot( GetSIDFromPID( Victim->GetPID( ) ), false );

				CONSOLE_Print( "[GAME: " + m_GameName + "] votekick against player [" + m_KickVotePlayer + "] passed with " + UTIL_ToString( Votes ) + "/" + UTIL_ToString( GetNumHumanPlayers( ) ) + " votes" );
				SendAllChat( m_GHost->m_Language->VoteKickPassed( m_KickVotePlayer ) );
			}
			else
				SendAllChat( m_GHost->m_Language->ErrorVoteKickingPlayer( m_KickVotePlayer ) );

			m_KickVotePlayer.clear( );
			m_StartedKickVoteTime = 0;
		}
		else
			SendAllChat( m_GHost->m_Language->VoteKickAcceptedNeedMoreVotes( m_KickVotePlayer, User, UTIL_ToString( VotesNeeded - Votes ) ) );
	}

	return HideCommand;
}

void CGame :: EventGameStarted( )
{
	CBaseGame :: EventGameStarted( );

	// record everything we need to ban each player in case we decide to do so later
	// this is because when a player leaves the game an admin might want to ban that player
	// but since the player has already left the game we don't have access to their information anymore
	// so we create a "potential ban" for each player and only store it in the database if requested to by an admin

	for( vector<CGamePlayer *> :: iterator i = m_Players.begin( ); i != m_Players.end( ); ++i )
		m_DBBans.push_back( new CDBBan( (*i)->GetJoinedRealm( ), (*i)->GetName( ), (*i)->GetExternalIPString( ), string( ), string( ), string( ), string( ) ) );
}

bool CGame :: IsGameDataSaved( )
{
	return m_CallableGameAdd && m_CallableGameAdd->GetReady( );
}

void CGame :: SaveGameData( )
{
	CONSOLE_Print( "[GAME: " + m_GameName + "] saving game data to database" );
	m_CallableGameAdd = m_GHost->m_DB->ThreadedGameAdd( m_GHost->m_BNETs.size( ) == 1 ? m_GHost->m_BNETs[0]->GetServer( ) : string( ), m_DBGame->GetMap( ), m_GameName, m_OwnerName, m_GameTicks / 1000, m_GameState, m_CreatorName, m_CreatorServer );
}


//New=============================================
void CBaseGame::ReCalculateTeams()
{
	unsigned char sid;
	unsigned char team;

	m_Team1 = 0;
	m_Team2 = 0;
	m_Team3 = 0;
	m_Team4 = 0;
	m_TeamDiff = 0;

	if (m_Players.empty())
		return;

	for (vector<CGamePlayer*> ::iterator i = m_Players.begin(); i != m_Players.end(); i++)
	{
		// ignore players who left and didn't get deleted yet.
		if (*i)
			if (!(*i)->GetLeftMessageSent())
			{
				sid = GetSIDFromPID((*i)->GetPID());
				if (sid != 255)
				{
					team = m_Slots[sid].GetTeam();
					if (team == 0)
						m_Team1++;
					if (team == 1)
						m_Team2++;
					if (team == 2)
						m_Team3++;
					if (team == 3)
						m_Team4++;
				}
			}
	}

	if (m_GetMapNumTeams == 2)
	{
		if (m_Team1 > m_Team2)
			m_TeamDiff = m_Team1 - m_Team2;
		if (m_Team1 < m_Team2)
			m_TeamDiff = m_Team2 - m_Team1;
	}

}
