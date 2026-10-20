#include "test_path.h"

#include <base/log.h>
#include <base/net.h>
#include <base/vmath.h>

#include <engine/client.h>

#include <game/client/gameclient.h>

// minimum movement between recorded points, in world units (32 = one tile)
static constexpr float TEST_PATH_MIN_STEP = 8.0f;
static constexpr size_t TEST_PATH_MAX_POINTS = 100000;

void CTestPath::OnStateChange(int NewState, int OldState)
{
	// a new connection starts a fresh path: only the last session is kept
	if(NewState == IClient::STATE_CONNECTING && OldState != IClient::STATE_CONNECTING)
	{
		m_vPath.clear();
		m_LoggedStart = false;
	}
}

void CTestPath::OnMapLoad()
{
	// covers switching the map through rcon on an already running local
	// server (no reconnect happens then)
	m_vPath.clear();
	m_LoggedStart = false;
}

void CTestPath::OnRender()
{
	UpdateRecording();
}

void CTestPath::UpdateRecording()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;
	if(!GameClient()->m_Snap.m_pLocalCharacter)
		return;

	const vec2 Pos(GameClient()->m_Snap.m_pLocalCharacter->m_X, GameClient()->m_Snap.m_pLocalCharacter->m_Y);
	if(!m_vPath.empty())
	{
		if(distance(m_vPath.back(), Pos) < TEST_PATH_MIN_STEP)
			return;
	}
	if(m_vPath.empty() && !m_LoggedStart)
	{
		m_LoggedStart = true;
		log_info("testpath", "recording local player path, start tile=(%.1f, %.1f)", Pos.x / 32.0f, Pos.y / 32.0f);
	}
	if(m_vPath.size() == 50)
		log_info("testpath", "path recording ongoing, %d points so far", (int)m_vPath.size());
	if(m_vPath.size() >= TEST_PATH_MAX_POINTS)
		m_vPath.erase(m_vPath.begin());
	m_vPath.push_back(Pos);
}
