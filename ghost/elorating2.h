#pragma once

#include "ghost.h"
#include "gameplayer.h"
#include "game_base.h"
#include "game.h"

class elorating2
{
public:
	static uint32_t CalcTeamCombinedRating(unsigned char team, vector<CGamePlayer*> m_Players, CGame* m_Game, vector<CGameSlot> m_Slots, uint32_t defaultPlayerRating, uint32_t* teamPlayersCount);

	static uint32_t CalcTeamCombinedRating(unsigned char team, vector<CGamePlayer*> m_Players, CGame* m_Game, vector<CGameSlot> m_Slots, uint32_t defaultPlayerRating);

	static uint32_t CalcTeamAvgRating(unsigned char team, vector<CGamePlayer*> m_Players, CGame* m_Game, vector<CGameSlot> m_Slots, uint32_t defaultPlayerRating, uint32_t& teamPlayersCount);

	static uint32_t CalcTeamAvgRating(unsigned char team, vector<CGamePlayer*> m_Players, CGame* m_Game, vector<CGameSlot> m_Slots, uint32_t defaultPlayerRating);

	/// <param name="r1">Player rating</param>
	/// <param name="r2">Opponent rating</param>
	/// <param name="eloGain">The Elo the player will gain upon game win</param>
	/// <param name="eloLoss">The Elo the player will lose if lost game</param>
	static void CalculateEloRatingChange(uint32_t r1, uint32_t r2, int32_t* eloGain, int32_t* eloLoss);

};

