
#include "ghost.h"
#include "gameplayer.h"
#include "game_base.h"
#include "game.h"
#include "elorating.h"

#include <cmath>

#include "next_combination.h"

uint32_t CalcTeamCombinedRating(unsigned char team, vector<CGamePlayer*> m_Players, CGame *m_Game, vector<CGameSlot> m_Slots, uint32_t defaultPlayerRating, int *teamPlayersCount)
{
	unsigned char TeamSizes[MAX_SLOTS];
	uint32_t res = 0;
	if (teamPlayersCount)
		teamPlayersCount = 0;

	for (vector<CGamePlayer*> ::iterator i = m_Players.begin(); i != m_Players.end(); ++i)
	{
		unsigned char PID = (*i)->GetPID();
		if (PID < 13)
		{
			unsigned char SID = m_Game->GetSIDFromPID(PID);
			if (SID < m_Slots.size())
			{
				unsigned char PlayerTeam = m_Slots[SID].GetTeam();
				if (PlayerTeam < MAX_SLOTS)
				{
					uint32_t Rating = (*i)->GetDotARating();
					if (Rating < -99999.0)
						Rating = defaultPlayerRating;

					TeamSizes[PlayerTeam]++;
					if (PlayerTeam == team)
					{
						res += Rating;
						if (teamPlayersCount)
							teamPlayersCount++;
					}						
				}
			}
		}
	}
	return res;
}

uint32_t CalcTeamCombinedRating(unsigned char team, vector<CGamePlayer*> m_Players, CGame* m_Game, vector<CGameSlot> m_Slots, uint32_t defaultPlayerRating)
{
	int x = 0;
	return CalcTeamCombinedRating(team, m_Players, m_Game, m_Slots, defaultPlayerRating, &x);
}

uint32_t CalcTeamAvgRating(unsigned char team, vector<CGamePlayer*> m_Players, CGame* m_Game, vector<CGameSlot> m_Slots, uint32_t defaultPlayerRating, int &teamPlayersCount)
{
	int playersInteam = 0;
	uint32_t combined = CalcTeamCombinedRating(team, m_Players, m_Game, m_Slots, defaultPlayerRating, &playersInteam);
	teamPlayersCount = playersInteam;
	return playersInteam > 0 ? combined / playersInteam : 0;
}

uint32_t CalcTeamAvgRating(unsigned char team, vector<CGamePlayer*> m_Players, CGame* m_Game, vector<CGameSlot> m_Slots, uint32_t defaultPlayerRating)
{
	int playersInteam = 0;
	uint32_t combined = CalcTeamCombinedRating(team, m_Players, m_Game, m_Slots, defaultPlayerRating, &playersInteam);
	return playersInteam > 0 ? combined / playersInteam : 0;
}

void CalculateEloRatingChange(uint32_t r1, uint32_t r2, int32_t* eloGain, int32_t* eloLoss)
{
    int K = 25;

    double R1 = pow(10, (double)r1 / 400);
    double R2 = pow(10, (double)r2 / 400);
    double E1 = R1 / (R1 + R2);
    double E2 = R2 / (R1 + R2);

    //Case player wins
    int S1 = 1;
    int S2 = 0;
    double r1New = r1 + K * (S1 - E1);
    double r2New = r2 + K * (S2 - E2);
    *eloGain = (int)lroundl(r1New - r1);

    //Case player loses
    S1 = 0;
    S2 = 1;
    r1New = r1 + K * (S1 - E1);
    r2New = r2 + K * (S2 - E2);
    *eloLoss = (int)lroundl(r1New - r1);
    return;
}
