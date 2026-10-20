#ifndef GAME_EDITOR_MAPITEMS_IMAGE_H
#define GAME_EDITOR_MAPITEMS_IMAGE_H

#include <base/types.h>

#include <engine/graphics.h>

#include <game/editor/auto_map.h>
#include <game/editor/map_object.h>

class CEditorImage : public CImageInfo, public CMapObject
{
public:
	explicit CEditorImage(CEditorMap *pMap);
	~CEditorImage() override;
	void OnAttach(CEditorMap *pMap) override;

	void AnalyseTileFlags();
	void Free();

	CEditorImage &operator=(CImageInfo &&Other);

	IGraphics::CTextureHandle m_Texture;
	int m_External = 0;
	char m_aName[IO_MAX_PATH_LENGTH] = "";
	unsigned char m_aTileFlags[256];

	// TClient: identifies the current pixel data; bumped on every data change
	// so the compressed-size cache can detect stale entries (globally unique,
	// never reused, survives nothing)
	uint64_t m_DataVersion = 0;
	void MarkDataModified();

	CAutomapper m_Automapper;

private:
	static uint64_t ms_NextDataVersion;
};

#endif
