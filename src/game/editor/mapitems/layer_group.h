#ifndef GAME_EDITOR_MAPITEMS_LAYER_GROUP_H
#define GAME_EDITOR_MAPITEMS_LAYER_GROUP_H

#include "layer.h"

#include <game/editor/map_object.h>

#include <memory>
#include <vector>

class CLayerGroup : public CMapObject
{
public:
	std::vector<std::shared_ptr<CLayer>> m_vpLayers;

	int m_OffsetX;
	int m_OffsetY;

	int m_ParallaxX;
	int m_ParallaxY;

	int m_UseClipping;
	int m_ClipX;
	int m_ClipY;
	int m_ClipW;
	int m_ClipH;

	char m_aName[12];
	bool m_GameGroup;
	bool m_Visible;
	bool m_Collapse;

	// transient editor-only color (cycled with Shift+left click in the layer toolbox),
	// index into EditorTheme::GROUP_COLORS or -1; not saved to the map format
	int m_ColorId = -1;

	explicit CLayerGroup(CEditorMap *pMap);
	void OnAttach(CEditorMap *pMap) override;

	void Convert(CUIRect *pRect) const;
	void Render(const CEditorMap *pRenderMap);
	void MapScreen();
	CScreenRect Mapping() const;

	void GetSize(float *pWidth, float *pHeight) const;

	void AddLayer(const std::shared_ptr<CLayer> &pLayer);
	void DeleteLayer(int Index);
	void DuplicateLayer(int Index);
	int MoveLayer(int IndexFrom, int IndexTo);

	bool IsEmpty() const;
	void Clear();

	void ModifyImageIndex(const FIndexModifyFunction &IndexModifyFunction);
	void ModifyEnvelopeIndex(const FIndexModifyFunction &IndexModifyFunction);
	void ModifySoundIndex(const FIndexModifyFunction &IndexModifyFunction);
};

#endif
