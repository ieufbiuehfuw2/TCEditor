#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_TEST_PATH_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_TEST_PATH_H

#include <base/vmath.h>

#include <game/client/component.h>

#include <vector>

// TClient: records the movement path of the local player while connected to
// a local server (the editor "test locally" flow) so the editor can draw the
// last session's path on the map.
class CTestPath : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }
	void OnStateChange(int NewState, int OldState) override;
	void OnMapLoad() override;
	void OnRender() override;

	// records the current position if the conditions are met; called from
	// the game render and from the editor render so the path also grows
	// live while the editor is open during a test session
	void UpdateRecording();

	const std::vector<vec2> &Path() const { return m_vPath; }
	void Clear() { m_vPath.clear(); }

private:
	std::vector<vec2> m_vPath;
	bool m_LoggedStart = false;
};

#endif
