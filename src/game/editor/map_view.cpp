#include "map_view.h"

#include "editor.h"

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/keys.h>
#include <engine/shared/config.h>

#include <game/client/animstate.h>
#include <game/client/render.h>
#include <game/client/ui.h>
#include <game/client/gameclient.h>
#include <game/editor/editor_actions.h>
#include <game/editor/explanations.h>

#include <base/color.h>

#include <cmath>

void CMapView::CState::Reset(CEditor *pEditor)
{
	m_Zoom = CSmoothValue(200.0f, 10.0f, 2000.0f);
	m_Zoom.OnInit(pEditor);
	m_WorldZoom = 1.0f;
	m_WorldOffset = vec2(0.0f, 0.0f);
	m_EditorOffset = vec2(0.0f, 0.0f);
	m_MouseWorldScale = 1.0f;
	m_MouseWorldPos = vec2(0.0f, 0.0f);
	m_MouseWorldNoParaPos = vec2(0.0f, 0.0f);
	m_MouseDeltaWorld = vec2(0.0f, 0.0f);
	m_ActiveOp = EActiveOp::NONE;
	m_LineStartWx = 0;
	m_LineStartWy = 0;
	m_LineEndWx = 0;
	m_LineEndWy = 0;
	m_LineWidth = 1;
}

void CMapView::OnInit(CEditor *pEditor)
{
	CEditorComponent::OnInit(pEditor);
	RegisterSubComponent(m_MapGrid);
	RegisterSubComponent(m_ProofMode);
	InitSubComponents();
}

void CMapView::OnMapLoad()
{
	m_ProofMode.OnMapLoad();
}

bool CMapView::IsFocused()
{
	return GetWorldOffset() == (m_ProofMode.IsModeMenu() ? m_ProofMode.CurrentMenuBackgroundPosition() : vec2(0.0f, 0.0f));
}

void CMapView::Focus()
{
	SetWorldOffset(m_ProofMode.IsModeMenu() ? m_ProofMode.CurrentMenuBackgroundPosition() : vec2(0.0f, 0.0f));
}

void CMapView::RenderGroupBorder()
{
	std::shared_ptr<CLayerGroup> pGroup = Map()->SelectedGroup();
	if(pGroup)
	{
		pGroup->MapScreen();

		for(size_t i = 0; i < Map()->m_vSelectedLayers.size(); i++)
		{
			std::shared_ptr<CLayer> pLayer = Map()->SelectedLayerType(i, LAYERTYPE_TILES);
			if(pLayer)
			{
				CUIRect BorderRect;
				BorderRect.x = 0.0f;
				BorderRect.y = 0.0f;
				pLayer->GetSize(&BorderRect.w, &BorderRect.h);
				BorderRect.DrawOutline(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
			}
		}
	}
}

void CMapView::RenderEditorMap()
{
	if(Editor()->m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr && Input()->ShiftIsPressed() && !Input()->ModifierIsPressed() && Input()->KeyPress(KEY_G))
	{
		const bool AnyHidden =
			!Map()->m_pGameLayer->m_Visible ||
			(Map()->m_pFrontLayer && !Map()->m_pFrontLayer->m_Visible) ||
			(Map()->m_pTeleLayer && !Map()->m_pTeleLayer->m_Visible) ||
			(Map()->m_pSpeedupLayer && !Map()->m_pSpeedupLayer->m_Visible) ||
			(Map()->m_pTuneLayer && !Map()->m_pTuneLayer->m_Visible) ||
			(Map()->m_pSwitchLayer && !Map()->m_pSwitchLayer->m_Visible);
		Map()->m_pGameLayer->m_Visible = AnyHidden;
		if(Map()->m_pFrontLayer)
			Map()->m_pFrontLayer->m_Visible = AnyHidden;
		if(Map()->m_pTeleLayer)
			Map()->m_pTeleLayer->m_Visible = AnyHidden;
		if(Map()->m_pSpeedupLayer)
			Map()->m_pSpeedupLayer->m_Visible = AnyHidden;
		if(Map()->m_pTuneLayer)
			Map()->m_pTuneLayer->m_Visible = AnyHidden;
		if(Map()->m_pSwitchLayer)
			Map()->m_pSwitchLayer->m_Visible = AnyHidden;
	}

	for(auto &pGroup : Map()->m_vpGroups)
	{
		if(pGroup->m_Visible)
			pGroup->Render(Map());
	}

	// render the game, tele, speedup, front, tune and switch above everything else
	if(Map()->m_pGameGroup->m_Visible)
	{
		Map()->m_pGameGroup->MapScreen();
		for(auto &pLayer : Map()->m_pGameGroup->m_vpLayers)
		{
			if(pLayer->m_Visible && pLayer->IsEntitiesLayer())
				pLayer->Render(Map());
		}
	}

	std::shared_ptr<CLayerTiles> pSelectedTilesLayer = std::static_pointer_cast<CLayerTiles>(Map()->SelectedLayerType(0, LAYERTYPE_TILES));
	if(Editor()->m_ShowTileInfo != CEditor::SHOW_TILE_OFF && pSelectedTilesLayer && pSelectedTilesLayer->m_Visible && Zoom()->GetValue() <= 300.0f)
	{
		Map()->SelectedGroup()->MapScreen();
		pSelectedTilesLayer->ShowInfo();
	}
}

void CMapView::Render(CUIRect View)
{
	// render all good stuff
	if(!Editor()->m_ShowPicker)
	{
		RenderEditorMap();
	}
	else
	{
		// fix aspect ratio of the image in the picker
		float Max = std::min(View.w, View.h);
		View.w = View.h = Max;
	}

	const bool Inside = Ui()->MouseInside(&View);

	// fetch mouse position
	float wx = MouseWorldPos().x;
	float wy = MouseWorldPos().y;
	float mx = Ui()->MouseX();
	float my = Ui()->MouseY();

	static float s_StartWx = 0;
	static float s_StartWy = 0;

	// remap the screen so it can display the whole tileset
	if(Editor()->m_ShowPicker)
	{
		CUIRect Screen = *Ui()->Screen();
		float Size = 32.0f * 16.0f;
		float w = Size * (Screen.w / View.w);
		float h = Size * (Screen.h / View.h);
		float x = -(View.x / Screen.w) * w;
		float y = -(View.y / Screen.h) * h;
		wx = x + w * mx / Screen.w;
		wy = y + h * my / Screen.h;
		std::shared_ptr<CLayerTiles> pTileLayer = std::static_pointer_cast<CLayerTiles>(Map()->SelectedLayerType(0, LAYERTYPE_TILES));
		if(pTileLayer)
		{
			CScreenRect ScreenRect(x, y, w, h);
			Graphics()->MapScreen(ScreenRect);
			Editor()->m_pTilesetPicker->m_Image = pTileLayer->m_Image;
			if(Editor()->m_BrushColorEnabled)
			{
				Editor()->m_pTilesetPicker->m_Color = pTileLayer->m_Color;
				Editor()->m_pTilesetPicker->m_Color.a = 255;
			}
			else
			{
				Editor()->m_pTilesetPicker->m_Color = {255, 255, 255, 255};
			}

			Editor()->m_pTilesetPicker->m_HasGame = pTileLayer->m_HasGame;
			Editor()->m_pTilesetPicker->m_HasTele = pTileLayer->m_HasTele;
			Editor()->m_pTilesetPicker->m_HasSpeedup = pTileLayer->m_HasSpeedup;
			Editor()->m_pTilesetPicker->m_HasFront = pTileLayer->m_HasFront;
			Editor()->m_pTilesetPicker->m_HasSwitch = pTileLayer->m_HasSwitch;
			Editor()->m_pTilesetPicker->m_HasTune = pTileLayer->m_HasTune;

			Editor()->m_pTilesetPicker->Render(Map());

			if(Editor()->m_ShowTileInfo != CEditor::SHOW_TILE_OFF)
				Editor()->m_pTilesetPicker->ShowInfo();

			str_copy(Editor()->m_aTooltip, "Click or drag left mouse button to create a brush. Hover individual tiles for explanation.");
		}
		else
		{
			std::shared_ptr<CLayerQuads> pQuadLayer = std::static_pointer_cast<CLayerQuads>(Map()->SelectedLayerType(0, LAYERTYPE_QUADS));
			if(pQuadLayer)
			{
				Editor()->m_pQuadsetPicker->m_Image = pQuadLayer->m_Image;
				Editor()->m_pQuadsetPicker->m_vQuads[0].m_aPoints[0].x = f2fx(View.x);
				Editor()->m_pQuadsetPicker->m_vQuads[0].m_aPoints[0].y = f2fx(View.y);
				Editor()->m_pQuadsetPicker->m_vQuads[0].m_aPoints[1].x = f2fx((View.x + View.w));
				Editor()->m_pQuadsetPicker->m_vQuads[0].m_aPoints[1].y = f2fx(View.y);
				Editor()->m_pQuadsetPicker->m_vQuads[0].m_aPoints[2].x = f2fx(View.x);
				Editor()->m_pQuadsetPicker->m_vQuads[0].m_aPoints[2].y = f2fx((View.y + View.h));
				Editor()->m_pQuadsetPicker->m_vQuads[0].m_aPoints[3].x = f2fx((View.x + View.w));
				Editor()->m_pQuadsetPicker->m_vQuads[0].m_aPoints[3].y = f2fx((View.y + View.h));
				Editor()->m_pQuadsetPicker->m_vQuads[0].m_aPoints[4].x = f2fx((View.x + View.w / 2));
				Editor()->m_pQuadsetPicker->m_vQuads[0].m_aPoints[4].y = f2fx((View.y + View.h / 2));
				Editor()->m_pQuadsetPicker->Render(Map());
			}
		}
	}

	// draw layer borders
	std::pair<int, std::shared_ptr<CLayer>> apEditLayers[128];
	size_t NumEditLayers = 0;

	if(Editor()->m_ShowPicker && Map()->SelectedLayer(0) && Map()->SelectedLayer(0)->m_Type == LAYERTYPE_TILES)
	{
		apEditLayers[0] = {0, Editor()->m_pTilesetPicker};
		NumEditLayers++;
	}
	else if(Editor()->m_ShowPicker)
	{
		apEditLayers[0] = {0, Editor()->m_pQuadsetPicker};
		NumEditLayers++;
	}
	else
	{
		// pick a type of layers to edit, preferring Tiles layers.
		int EditingType = -1;
		for(size_t i = 0; i < Map()->m_vSelectedLayers.size(); i++)
		{
			std::shared_ptr<CLayer> pLayer = Map()->SelectedLayer(i);
			if(pLayer && (EditingType == -1 || pLayer->m_Type == LAYERTYPE_TILES))
			{
				EditingType = pLayer->m_Type;
				if(EditingType == LAYERTYPE_TILES)
					break;
			}
		}
		for(size_t i = 0; i < Map()->m_vSelectedLayers.size() && NumEditLayers < 128; i++)
		{
			apEditLayers[NumEditLayers] = {Map()->m_vSelectedLayers[i], Map()->SelectedLayerType(i, EditingType)};
			if(apEditLayers[NumEditLayers].second)
			{
				NumEditLayers++;
			}
		}

		RenderGroupBorder();
		MapGrid()->Render();

		// TClient: draw the movement path of the last local test session
		// (recorded while the player was on the local test server); the line
		// color and width come from the background draw settings. The local
		// player is also drawn at their live position during a test session.
		if(!Editor()->m_ShowPicker && Editor()->m_ShowTestPath && Map()->m_pGameGroup)
		{
			CGameClient *pGameClient = (CGameClient *)Editor()->Kernel()->RequestInterface<IGameClient>();
			pGameClient->m_TestPath.UpdateRecording();
			const std::vector<vec2> &vPath = pGameClient->m_TestPath.Path();
			if(vPath.size() >= 2)
			{
				Map()->m_pGameGroup->MapScreen();
				const ColorRGBA PathColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcBgDrawColor)).WithAlpha(0.8f);
				const float PathWidth = (float)g_Config.m_TcBgDrawWidth;
				Graphics()->TextureClear();
				Graphics()->QuadsBegin();
				Graphics()->SetColor(PathColor);
				for(size_t i = 1; i < vPath.size(); i++)
				{
					const vec2 From = vPath[i - 1];
					const vec2 To = vPath[i];
					const vec2 Dir = To - From;
					const float Len = length(Dir);
					// draw the segment as overlapping squares so the line is
					// continuous and has the configured thickness
					const int Steps = std::max(1, (int)std::ceil(Len / std::max(1.0f, PathWidth / 2.0f)));
					for(int s = 0; s <= Steps; s++)
					{
						const vec2 Point = From + Dir * ((float)s / Steps);
						const IGraphics::CQuadItem Item(Point.x - PathWidth / 2.0f, Point.y - PathWidth / 2.0f, PathWidth, PathWidth);
						Graphics()->QuadsDrawTL(&Item, 1);
					}
				}
				Graphics()->QuadsEnd();
			}

			// live local player during a test session
			const CNetObj_Character *pLocalChar = pGameClient->m_Snap.m_pLocalCharacter;
			const CNetObj_PlayerInfo *pLocalInfo = pGameClient->m_Snap.m_pLocalInfo;
			if(pLocalChar && pLocalInfo && Client()->State() == IClient::STATE_ONLINE)
			{
				Map()->m_pGameGroup->MapScreen();
				CTeeRenderInfo Info = pGameClient->m_aClients[pLocalInfo->m_ClientId].m_RenderInfo;
				Info.m_Size = 64.0f;
				const vec2 PlayerPos(pLocalChar->m_X, pLocalChar->m_Y);
				const vec2 PlayerDir = direction(pLocalChar->m_Angle / 256.0f);
				pGameClient->RenderTools()->RenderTee(CAnimState::GetIdle(), &Info, pLocalChar->m_Emote, PlayerDir, PlayerPos, 0.9f);
			}
		}
	}

	// stamps the brush at the given world position, shared by the freehand
	// brush draw and the line tool (TClient)
	auto StampBrush = [&](float StampWx, float StampWy) {
		if(Editor()->m_pBrush->IsEmpty())
			return;
		for(size_t k = 0; k < NumEditLayers; k++)
		{
			size_t BrushIndex = k % Editor()->m_pBrush->m_vpLayers.size();
			if(apEditLayers[k].second->m_Type == Editor()->m_pBrush->m_vpLayers[BrushIndex]->m_Type)
			{
				if(apEditLayers[k].second->m_Type == LAYERTYPE_TILES)
				{
					std::shared_ptr<CLayerTiles> pLayer = std::static_pointer_cast<CLayerTiles>(apEditLayers[k].second);
					std::shared_ptr<CLayerTiles> pBrushLayer = std::static_pointer_cast<CLayerTiles>(Editor()->m_pBrush->m_vpLayers[BrushIndex]);

					if((!pLayer->m_HasTele || pBrushLayer->m_HasTele) && (!pLayer->m_HasSpeedup || pBrushLayer->m_HasSpeedup) && (!pLayer->m_HasFront || pBrushLayer->m_HasFront) && (!pLayer->m_HasGame || pBrushLayer->m_HasGame) && (!pLayer->m_HasSwitch || pBrushLayer->m_HasSwitch) && (!pLayer->m_HasTune || pBrushLayer->m_HasTune))
						pLayer->BrushDraw(pBrushLayer.get(), vec2(StampWx, StampWy));
				}
				else
				{
					apEditLayers[k].second->BrushDraw(Editor()->m_pBrush->m_vpLayers[BrushIndex].get(), vec2(StampWx, StampWy));
				}
			}
		}
	};

	// TClient: tiles of the line tool for a line from tile A to tile B with
	// the given thickness in tiles (spread perpendicular to the line)
	auto ComputeLineTiles = [](int StartTx, int StartTy, int EndTx, int EndTy, int Width) {
		std::vector<std::pair<int, int>> vTiles;
		const int Dx = EndTx - StartTx;
		const int Dy = EndTy - StartTy;
		const int Steps = std::max(std::abs(Dx), std::abs(Dy));
		float PerpX = 0.0f;
		float PerpY = 0.0f;
		if(Steps > 0)
		{
			const float Len = std::sqrt((float)(Dx * Dx + Dy * Dy));
			PerpX = -(float)Dy / Len;
			PerpY = (float)Dx / Len;
		}
		for(int i = 0; i <= Steps; i++)
		{
			const int Tx = Steps > 0 ? StartTx + (Dx * i + Steps / 2) / Steps : StartTx;
			const int Ty = Steps > 0 ? StartTy + (Dy * i + Steps / 2) / Steps : StartTy;
			for(int k = 0; k < Width; k++)
				vTiles.emplace_back(Tx + (int)std::round(k * PerpX), Ty + (int)std::round(k * PerpY));
		}
		return vTiles;
	};

	// TClient: LCtrl+left mouse is reserved for the line tool, only RCtrl
	// (or middle mouse) pans the view
	const bool ShouldPan = Ui()->HotItem() == Editor()->MapView() && ((Input()->KeyIsPressed(KEY_RCTRL) && Ui()->MouseButton(0)) || Ui()->MouseButton(2));
	if(Editor()->m_pContainerPanned == Editor()->MapView())
	{
		// do panning
		if(ShouldPan)
		{
			if(Input()->ShiftIsPressed())
				Map()->m_MapViewState.m_ActiveOp = EActiveOp::PAN_EDITOR;
			else
				Map()->m_MapViewState.m_ActiveOp = EActiveOp::PAN_WORLD;
			Ui()->SetActiveItem(Editor()->MapView());
		}
		else
			Map()->m_MapViewState.m_ActiveOp = EActiveOp::NONE;

		if(Map()->m_MapViewState.m_ActiveOp == EActiveOp::PAN_WORLD)
			OffsetWorld(-Ui()->MouseDelta() * MouseWorldScale());
		else if(Map()->m_MapViewState.m_ActiveOp == EActiveOp::PAN_EDITOR)
			OffsetEditor(-Ui()->MouseDelta() * MouseWorldScale());

		if(Map()->m_MapViewState.m_ActiveOp == EActiveOp::NONE)
			Editor()->m_pContainerPanned = nullptr;
	}

	if(Inside)
	{
		Ui()->SetHotItem(Editor()->MapView());

		// do global operations like pan and zoom
		if(Ui()->CheckActiveItem(nullptr) && (Ui()->MouseButton(0) || Ui()->MouseButton(2)))
		{
			s_StartWx = wx;
			s_StartWy = wy;

			if(ShouldPan && Editor()->m_pContainerPanned == nullptr)
				Editor()->m_pContainerPanned = Editor()->MapView();
		}

		// brush editing
		if(Ui()->HotItem() == Editor()->MapView())
		{
			if(Editor()->m_ShowPicker)
			{
				std::shared_ptr<CLayer> pLayer = Map()->SelectedLayer(0);
				int Layer;
				if(pLayer == Map()->m_pGameLayer)
					Layer = LAYER_GAME;
				else if(pLayer == Map()->m_pFrontLayer)
					Layer = LAYER_FRONT;
				else if(pLayer == Map()->m_pSwitchLayer)
					Layer = LAYER_SWITCH;
				else if(pLayer == Map()->m_pTeleLayer)
					Layer = LAYER_TELE;
				else if(pLayer == Map()->m_pSpeedupLayer)
					Layer = LAYER_SPEEDUP;
				else if(pLayer == Map()->m_pTuneLayer)
					Layer = LAYER_TUNE;
				else
					Layer = NUM_LAYERS;

				CExplanations::EGametype ExplanationGametype;
				if(Editor()->m_SelectEntitiesImage == "DDNet")
					ExplanationGametype = CExplanations::EGametype::DDNET;
				else if(Editor()->m_SelectEntitiesImage == "FNG")
					ExplanationGametype = CExplanations::EGametype::FNG;
				else if(Editor()->m_SelectEntitiesImage == "Race")
					ExplanationGametype = CExplanations::EGametype::RACE;
				else if(Editor()->m_SelectEntitiesImage == "Vanilla")
					ExplanationGametype = CExplanations::EGametype::VANILLA;
				else if(Editor()->m_SelectEntitiesImage == "blockworlds")
					ExplanationGametype = CExplanations::EGametype::BLOCKWORLDS;
				else
					ExplanationGametype = CExplanations::EGametype::NONE;

				if(Layer != NUM_LAYERS)
				{
					const char *pExplanation = CExplanations::Explain(ExplanationGametype, (int)wx / 32 + (int)wy / 32 * 16, Layer);
					if(pExplanation)
						str_copy(Editor()->m_aTooltip, pExplanation);
				}
			}
			else if(Editor()->m_pBrush->IsEmpty() && Map()->SelectedLayerType(0, LAYERTYPE_QUADS) != nullptr)
				str_copy(Editor()->m_aTooltip, "Use left mouse button to drag and create a brush. Hold shift to select multiple quads. Press R to rotate selected quads. Use ctrl+right click to select layer.");
			else if(Editor()->m_pBrush->IsEmpty())
			{
				if(g_Config.m_EdLayerSelector)
					str_copy(Editor()->m_aTooltip, "Use left mouse button to drag and create a brush. Use ctrl+right click to select layer of hovered tile.");
				else
					str_copy(Editor()->m_aTooltip, "Use left mouse button to drag and create a brush.");
			}
			else
			{
				// Alt behavior handled in CEditor::MouseAxisLock
				if(Editor()->m_BucketFillMode)
					str_copy(Editor()->m_aTooltip, "Bucket fill active: click to fill the connected region under the cursor with the brush tile. Press F or the Bucket button to turn it off.");
				else
					str_copy(Editor()->m_aTooltip, "Use left mouse button to paint with the brush. Right click to clear the brush. Hold Alt to lock the mouse movement to a single axis. Hold LCtrl with the left mouse button to draw a straight line of tiles, scroll to change its width.");
			}

			if(Ui()->CheckActiveItem(Editor()->MapView()))
			{
				CUIRect r;
				r.x = s_StartWx;
				r.y = s_StartWy;
				r.w = wx - s_StartWx;
				r.h = wy - s_StartWy;
				if(r.w < 0)
				{
					r.x += r.w;
					r.w = -r.w;
				}

				if(r.h < 0)
				{
					r.y += r.h;
					r.h = -r.h;
				}

				if(Map()->m_MapViewState.m_ActiveOp == EActiveOp::BRUSH_DRAW)
				{
					if(!Editor()->m_pBrush->IsEmpty())
						StampBrush(wx, wy);
				}
				else if(Map()->m_MapViewState.m_ActiveOp == EActiveOp::BRUSH_LINE)
				{
					// TClient: preview the line while dragging — the tiles are
					// only placed when the mouse is released, so the line can
					// be adjusted while LCtrl and the left button are held.
					// The mouse wheel adjusts the thickness (at least 1 tile).
					Map()->m_MapViewState.m_LineEndWx = (int)wx;
					Map()->m_MapViewState.m_LineEndWy = (int)wy;
					if(Input()->KeyPress(KEY_MOUSE_WHEEL_UP))
						Map()->m_MapViewState.m_LineWidth++;
					if(Input()->KeyPress(KEY_MOUSE_WHEEL_DOWN))
						Map()->m_MapViewState.m_LineWidth = std::max(1, Map()->m_MapViewState.m_LineWidth - 1);

					if(!Editor()->m_pBrush->IsEmpty())
					{
						const auto vTiles = ComputeLineTiles(
							Map()->m_MapViewState.m_LineStartWx / 32,
							Map()->m_MapViewState.m_LineStartWy / 32,
							(int)wx / 32,
							(int)wy / 32,
							Map()->m_MapViewState.m_LineWidth);

						for(const auto &[Tx, Ty] : vTiles)
						{
							const float TileX = (float)Tx * 32.0f;
							const float TileY = (float)Ty * 32.0f;
							Graphics()->DrawRect(TileX, TileY, 32.0f, 32.0f, ColorRGBA(1.0f, 1.0f, 1.0f, 0.35f), IGraphics::CORNER_NONE, 0.0f);
							const IGraphics::CLineItem aLines[] = {
								IGraphics::CLineItem(TileX, TileY, TileX + 32.0f, TileY),
								IGraphics::CLineItem(TileX + 32.0f, TileY, TileX + 32.0f, TileY + 32.0f),
								IGraphics::CLineItem(TileX + 32.0f, TileY + 32.0f, TileX, TileY + 32.0f),
								IGraphics::CLineItem(TileX, TileY + 32.0f, TileX, TileY)};
							Graphics()->TextureClear();
							Graphics()->LinesBegin();
							Graphics()->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
							Graphics()->LinesDraw(aLines, std::size(aLines));
							Graphics()->LinesEnd();
						}
					}
				}
				else if(Map()->m_MapViewState.m_ActiveOp == EActiveOp::BRUSH_GRAB)
				{
					if(!Ui()->MouseButton(0))
					{
						std::shared_ptr<CLayerQuads> pQuadLayer = std::static_pointer_cast<CLayerQuads>(Map()->SelectedLayerType(0, LAYERTYPE_QUADS));
						if(Input()->ShiftIsPressed() && pQuadLayer)
						{
							Map()->DeselectQuads();
							for(size_t i = 0; i < pQuadLayer->m_vQuads.size(); i++)
							{
								const CQuad &Quad = pQuadLayer->m_vQuads[i];
								vec2 Position = vec2(fx2f(Quad.m_aPoints[4].x), fx2f(Quad.m_aPoints[4].y));
								if(r.Inside(Position) && !Map()->IsQuadSelected(i))
									Map()->ToggleSelectQuad(i);
							}
						}
						else
						{
							// TODO: do all layers
							int Grabs = 0;
							for(size_t k = 0; k < NumEditLayers; k++)
								Grabs += apEditLayers[k].second->BrushGrab(Editor()->m_pBrush.get(), r);
							if(Grabs == 0)
								Editor()->m_pBrush->Clear();

							Editor()->m_ShowPickerToggle = false; // Close the tile picker after grabbing brush if it was toggled open
							Map()->DeselectQuads();
							Map()->DeselectQuadPoints();
						}
					}
					else
					{
						if(NumEditLayers > 0)
						{
							apEditLayers[0].second->BrushSelecting(r);
						}
						Ui()->MapScreen();
					}
				}
				else if(Map()->m_MapViewState.m_ActiveOp == EActiveOp::BRUSH_PAINT)
				{
					if(!Ui()->MouseButton(0))
					{
						for(size_t k = 0; k < NumEditLayers; k++)
						{
							size_t BrushIndex = k;
							if(Editor()->m_pBrush->m_vpLayers.size() != NumEditLayers)
								BrushIndex = 0;
							std::shared_ptr<CLayer> pBrush = Editor()->m_pBrush->IsEmpty() ? nullptr : Editor()->m_pBrush->m_vpLayers[BrushIndex];
							apEditLayers[k].second->FillSelection(Editor()->m_pBrush->IsEmpty(), pBrush.get(), r);
						}
						std::shared_ptr<IEditorAction> Action = std::make_shared<CEditorBrushDrawAction>(Map(), Map()->m_SelectedGroup);
						Map()->m_EditorHistory.RecordAction(Action);
					}
					else
					{
						if(NumEditLayers > 0)
						{
							apEditLayers[0].second->BrushSelecting(r);
						}
						Ui()->MapScreen();
					}
				}
			}
			else
			{
				if(Ui()->MouseButton(1))
				{
					Editor()->m_pBrush->Clear();
				}

				// TClient: bucket fill tools consume the click when active
				if(Editor()->m_BucketEEnabled && !Editor()->m_pBrush->IsEmpty() && !Input()->ModifierIsPressed() && !Input()->ShiftIsPressed() && Ui()->MouseButton(0) && Map()->m_MapViewState.m_ActiveOp == EActiveOp::NONE && !Editor()->QuadKnife()->IsActive() && !Editor()->m_ShowPicker)
				{
					if(Ui()->CheckActiveItem(nullptr))
					{
						Ui()->SetActiveItem(Editor()->MapView());
						BucketFillE(wx, wy);
					}
				}
				else if(Editor()->m_BucketFillMode && !Editor()->m_pBrush->IsEmpty() && !Input()->ModifierIsPressed() && !Input()->ShiftIsPressed() && Ui()->MouseButton(0) && Map()->m_MapViewState.m_ActiveOp == EActiveOp::NONE && !Editor()->QuadKnife()->IsActive() && !Editor()->m_ShowPicker)
				{
					if(Ui()->CheckActiveItem(nullptr))
					{
						Ui()->SetActiveItem(Editor()->MapView());
						BucketFill(wx, wy);
					}
				}
				else if(!Input()->ModifierIsPressed() && Ui()->MouseButton(0) && Map()->m_MapViewState.m_ActiveOp == EActiveOp::NONE && !Editor()->QuadKnife()->IsActive())
				{
					Ui()->SetActiveItem(Editor()->MapView());

					if(Editor()->m_pBrush->IsEmpty())
						Map()->m_MapViewState.m_ActiveOp = EActiveOp::BRUSH_GRAB;
					else
					{
						Map()->m_MapViewState.m_ActiveOp = EActiveOp::BRUSH_DRAW;
						for(size_t k = 0; k < NumEditLayers; k++)
						{
							size_t BrushIndex = k;
							if(Editor()->m_pBrush->m_vpLayers.size() != NumEditLayers)
								BrushIndex = 0;

							if(apEditLayers[k].second->m_Type == Editor()->m_pBrush->m_vpLayers[BrushIndex]->m_Type)
								apEditLayers[k].second->BrushPlace(Editor()->m_pBrush->m_vpLayers[BrushIndex].get(), vec2(wx, wy));
						}
					}

					std::shared_ptr<CLayerTiles> pLayer = std::static_pointer_cast<CLayerTiles>(Map()->SelectedLayerType(0, LAYERTYPE_TILES));
					if(Input()->ShiftIsPressed() && pLayer)
						Map()->m_MapViewState.m_ActiveOp = EActiveOp::BRUSH_PAINT;
				}

				// TClient: line tool — hold LCtrl and left-click drag to draw
				// a straight line of brush tiles from the start point to the
				// mouse; scroll while dragging to change the line thickness
				if(Input()->KeyIsPressed(KEY_LCTRL) && Ui()->MouseButton(0) && !Input()->KeyIsPressed(KEY_RCTRL) && Map()->m_MapViewState.m_ActiveOp == EActiveOp::NONE && !Editor()->QuadKnife()->IsActive() && !Editor()->m_pBrush->IsEmpty() && !Editor()->m_ShowPicker)
				{
					Ui()->SetActiveItem(Editor()->MapView());
					Map()->m_MapViewState.m_ActiveOp = EActiveOp::BRUSH_LINE;
					Map()->m_MapViewState.m_LineStartWx = (int)wx;
					Map()->m_MapViewState.m_LineStartWy = (int)wy;
					Map()->m_MapViewState.m_LineEndWx = (int)wx;
					Map()->m_MapViewState.m_LineEndWy = (int)wy;
				}

				if(!Editor()->m_pBrush->IsEmpty())
				{
					Editor()->m_pBrush->m_OffsetX = -(int)wx;
					Editor()->m_pBrush->m_OffsetY = -(int)wy;
					for(const auto &pLayer : Editor()->m_pBrush->m_vpLayers)
					{
						if(pLayer->m_Type == LAYERTYPE_TILES)
						{
							Editor()->m_pBrush->m_OffsetX = -(int)(wx / 32.0f) * 32;
							Editor()->m_pBrush->m_OffsetY = -(int)(wy / 32.0f) * 32;
							break;
						}
					}

					std::shared_ptr<CLayerGroup> pGroup = Map()->SelectedGroup();
					if(!Editor()->m_ShowPicker && pGroup)
					{
						Editor()->m_pBrush->m_OffsetX += pGroup->m_OffsetX;
						Editor()->m_pBrush->m_OffsetY += pGroup->m_OffsetY;
						Editor()->m_pBrush->m_ParallaxX = pGroup->m_ParallaxX;
						Editor()->m_pBrush->m_ParallaxY = pGroup->m_ParallaxY;
						Editor()->m_pBrush->Render(Map());

						CUIRect BorderRect;
						BorderRect.x = 0.0f;
						BorderRect.y = 0.0f;
						Editor()->m_pBrush->GetSize(&BorderRect.w, &BorderRect.h);
						BorderRect.DrawOutline(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
					}
				}
			}
		}

		// quad & sound editing
		{
			if(!Editor()->m_ShowPicker && Editor()->m_pBrush->IsEmpty())
			{
				// fetch layers
				std::shared_ptr<CLayerGroup> pGroup = Map()->SelectedGroup();
				if(pGroup)
					pGroup->MapScreen();

				for(size_t k = 0; k < NumEditLayers; k++)
				{
					auto &[LayerIndex, pEditLayer] = apEditLayers[k];

					if(pEditLayer->m_Type == LAYERTYPE_QUADS)
					{
						std::shared_ptr<CLayerQuads> pLayer = std::static_pointer_cast<CLayerQuads>(pEditLayer);

						if(Editor()->m_ActiveEnvelopePreview == CEditor::EEnvelopePreview::NONE)
							Editor()->m_ActiveEnvelopePreview = CEditor::EEnvelopePreview::ALL;

						if(Editor()->QuadKnife()->IsActive())
						{
							Editor()->QuadKnife()->DoSlice();
						}
						else
						{
							Editor()->UpdateHotQuadPoint(pLayer.get());

							Graphics()->TextureClear();
							Graphics()->QuadsBegin();
							for(size_t i = 0; i < pLayer->m_vQuads.size(); i++)
							{
								for(int v = 0; v < 4; v++)
									Editor()->DoQuadPoint(LayerIndex, pLayer, &pLayer->m_vQuads[i], i, v);

								Editor()->DoQuad(LayerIndex, pLayer, &pLayer->m_vQuads[i], i);
							}
							Graphics()->QuadsEnd();
						}
					}
					else if(pEditLayer->m_Type == LAYERTYPE_SOUNDS)
					{
						std::shared_ptr<CLayerSounds> pLayer = std::static_pointer_cast<CLayerSounds>(pEditLayer);

						Editor()->UpdateHotSoundSource(pLayer.get());

						Graphics()->TextureClear();
						Graphics()->QuadsBegin();
						for(size_t i = 0; i < pLayer->m_vSources.size(); i++)
						{
							Editor()->DoSoundSource(LayerIndex, &pLayer->m_vSources[i], i);
						}
						Graphics()->QuadsEnd();
					}
				}

				Ui()->MapScreen();
			}
		}

		// menu proof selection
		if(ProofMode()->IsModeMenu() && !Editor()->m_ShowPicker)
		{
			ProofMode()->InitMenuBackgroundPositions();
			const std::vector<vec2> &MenuBackgroundPositions = ProofMode()->MenuBackgroundPositions();
			for(int i = 0; i < (int)MenuBackgroundPositions.size(); i++)
			{
				vec2 Pos = MenuBackgroundPositions[i];
				const void *pId = &MenuBackgroundPositions[i];
				Pos += GetWorldOffset() - MenuBackgroundPositions[ProofMode()->CurrentMenuProofIndex()];
				Pos.y -= 3.0f;

				if(distance(Pos, MouseWorldNoParaPos()) <= 20.0f)
				{
					Ui()->SetHotItem(pId);

					if(i != ProofMode()->CurrentMenuProofIndex() && Ui()->CheckActiveItem(pId))
					{
						if(!Ui()->MouseButton(0))
						{
							ProofMode()->SetCurrentMenuProofIndex(i);
							SetWorldOffset(MenuBackgroundPositions[i]);
							Ui()->SetActiveItem(nullptr);
						}
					}
					else if(Ui()->HotItem() == pId)
					{
						char aTooltipPrefix[32] = "Switch proof position to";
						if(i == ProofMode()->CurrentMenuProofIndex())
							str_copy(aTooltipPrefix, "Current proof position at");

						char aNumBuf[8];
						if(i < (TILE_TIME_CHECKPOINT_LAST - TILE_TIME_CHECKPOINT_FIRST))
							str_format(aNumBuf, sizeof(aNumBuf), "#%d", i + 1);
						else
							aNumBuf[0] = '\0';

						char aTooltipPositions[128];
						str_format(aTooltipPositions, sizeof(aTooltipPositions), "%s %s", ProofMode()->MenuBackgroundPositionName(i), aNumBuf);

						for(int k : ProofMode()->MenuBackgroundCollisions(i))
						{
							if(k == ProofMode()->CurrentMenuProofIndex())
								str_copy(aTooltipPrefix, "Current proof position at");

							Pos = MenuBackgroundPositions[k];
							Pos += GetWorldOffset() - MenuBackgroundPositions[ProofMode()->CurrentMenuProofIndex()];
							Pos.y -= 3.0f;

							if(distance(Pos, MouseWorldNoParaPos()) > 20.0f)
								continue;

							if(i < (TILE_TIME_CHECKPOINT_LAST - TILE_TIME_CHECKPOINT_FIRST))
								str_format(aNumBuf, sizeof(aNumBuf), "#%d", k + 1);
							else
								aNumBuf[0] = '\0';

							char aTooltipPositionsCopy[128];
							str_copy(aTooltipPositionsCopy, aTooltipPositions);
							str_format(aTooltipPositions, sizeof(aTooltipPositions), "%s, %s %s", aTooltipPositionsCopy, ProofMode()->MenuBackgroundPositionName(k), aNumBuf);
						}
						str_format(Editor()->m_aTooltip, sizeof(Editor()->m_aTooltip), "%s %s.", aTooltipPrefix, aTooltipPositions);

						if(Ui()->MouseButton(0))
							Ui()->SetActiveItem(pId);
					}
					break;
				}
			}
		}

		if(!Input()->ModifierIsPressed() && Editor()->m_Dialog == DIALOG_NONE && CLineInput::GetActiveInput() == nullptr)
		{
			float PanSpeed = Input()->ShiftIsPressed() ? 200.0f : 64.0f;
			if(Input()->KeyPress(KEY_A))
				OffsetWorld({-PanSpeed * MouseWorldScale(), 0});
			else if(Input()->KeyPress(KEY_D))
				OffsetWorld({PanSpeed * MouseWorldScale(), 0});
			if(Input()->KeyPress(KEY_W))
				OffsetWorld({0, -PanSpeed * MouseWorldScale()});
			else if(Input()->KeyPress(KEY_S))
				OffsetWorld({0, PanSpeed * MouseWorldScale()});
		}
	}

	if(Ui()->CheckActiveItem(Editor()->MapView()) && Editor()->m_pContainerPanned == nullptr)
	{
		// release mouse
		if(!Ui()->MouseButton(0))
		{
			if(Map()->m_MapViewState.m_ActiveOp == EActiveOp::BRUSH_DRAW)
			{
				std::shared_ptr<IEditorAction> pAction = std::make_shared<CEditorBrushDrawAction>(Map(), Map()->m_SelectedGroup);

				if(!pAction->IsEmpty()) // Avoid recording tile draw action when placing quads only
					Map()->m_EditorHistory.RecordAction(pAction);
			}
			else if(Map()->m_MapViewState.m_ActiveOp == EActiveOp::BRUSH_LINE)
			{
				// TClient: the line was previewed during the drag, place its
				// tiles now
				const auto vTiles = ComputeLineTiles(
					Map()->m_MapViewState.m_LineStartWx / 32,
					Map()->m_MapViewState.m_LineStartWy / 32,
					Map()->m_MapViewState.m_LineEndWx / 32,
					Map()->m_MapViewState.m_LineEndWy / 32,
					Map()->m_MapViewState.m_LineWidth);
				for(const auto &[Tx, Ty] : vTiles)
					StampBrush((float)Tx * 32.0f, (float)Ty * 32.0f);

				std::shared_ptr<IEditorAction> pAction = std::make_shared<CEditorBrushDrawAction>(Map(), Map()->m_SelectedGroup);
				if(!pAction->IsEmpty())
					Map()->m_EditorHistory.RecordAction(pAction);
			}

			Map()->m_MapViewState.m_ActiveOp = EActiveOp::NONE;
			Ui()->SetActiveItem(nullptr);
		}
	}

	if(!Editor()->m_ShowPicker && Map()->SelectedGroup() && Map()->SelectedGroup()->m_UseClipping)
	{
		std::shared_ptr<CLayerGroup> pGameGroup = Map()->m_pGameGroup;
		pGameGroup->MapScreen();

		CUIRect ClipRect;
		ClipRect.x = Map()->SelectedGroup()->m_ClipX;
		ClipRect.y = Map()->SelectedGroup()->m_ClipY;
		ClipRect.w = Map()->SelectedGroup()->m_ClipW;
		ClipRect.h = Map()->SelectedGroup()->m_ClipH;
		ClipRect.DrawOutline(ColorRGBA(1.0f, 0.0f, 0.0f, 1.0f));
	}

	if(!Editor()->m_ShowPicker)
		ProofMode()->RenderScreenSizes();

	if(!Editor()->m_ShowPicker && Editor()->m_ShowEnvelopePreview && Editor()->m_ActiveEnvelopePreview != CEditor::EEnvelopePreview::NONE)
	{
		const std::shared_ptr<CLayer> pSelectedLayer = Map()->SelectedLayer(0);
		if(pSelectedLayer != nullptr && pSelectedLayer->m_Type == LAYERTYPE_QUADS)
		{
			Editor()->DoQuadEnvelopes(static_cast<const CLayerQuads *>(pSelectedLayer.get()));
		}
		Editor()->m_ActiveEnvelopePreview = CEditor::EEnvelopePreview::NONE;
	}

	Ui()->MapScreen();
}

// TClient: bucket fill — flood fills the region of tiles matching the
// clicked tile with the brush tile, on every selected tiles layer. Filling
// air works too: only the connected region is filled, like in paint programs.
void CMapView::BucketFill(float Wx, float Wy)
{
	if(Editor()->m_pBrush->IsEmpty())
		return;

	std::shared_ptr<CLayerTiles> pBrushTiles;
	for(const auto &pBrushLayer : Editor()->m_pBrush->m_vpLayers)
	{
		if(pBrushLayer->m_Type == LAYERTYPE_TILES)
		{
			pBrushTiles = std::static_pointer_cast<CLayerTiles>(pBrushLayer);
			break;
		}
	}
	if(!pBrushTiles)
		return;

	bool Filled = false;
	for(size_t i = 0; i < Map()->m_vSelectedLayers.size(); i++)
	{
		std::shared_ptr<CLayer> pSelectedLayer = Map()->SelectedLayer(i);
		if(!pSelectedLayer || pSelectedLayer->m_Type != LAYERTYPE_TILES)
			continue;
		std::shared_ptr<CLayerTiles> pTiles = std::static_pointer_cast<CLayerTiles>(pSelectedLayer);

		const int FillX = (int)Wx / 32;
		const int FillY = (int)Wy / 32;
		if(FillX < 0 || FillX >= pTiles->m_Width || FillY < 0 || FillY >= pTiles->m_Height)
			continue;

		const CTile Target = pTiles->GetTile(FillX, FillY);
		const CTile Replace = pBrushTiles->GetTile(0, 0);
		// nothing to do when the region already has the brush tile
		if(Target.m_Index == Replace.m_Index && Target.m_Flags == Replace.m_Flags)
			continue;

		std::vector<bool> vVisited((size_t)pTiles->m_Width * pTiles->m_Height, false);
		std::vector<int> vStack;
		vStack.push_back(FillY * pTiles->m_Width + FillX);
		while(!vStack.empty())
		{
			const int Pos = vStack.back();
			vStack.pop_back();
			if(vVisited[Pos])
				continue;
			vVisited[Pos] = true;

			const int X = Pos % pTiles->m_Width;
			const int Y = Pos / pTiles->m_Width;
			const CTile Tile = pTiles->GetTile(X, Y);
			if(Tile.m_Index != Target.m_Index || Tile.m_Flags != Target.m_Flags)
				continue;

			pTiles->SetTile(X, Y, Replace);

			if(X > 0)
				vStack.push_back(Pos - 1);
			if(X < pTiles->m_Width - 1)
				vStack.push_back(Pos + 1);
			if(Y > 0)
				vStack.push_back(Pos - pTiles->m_Width);
			if(Y < pTiles->m_Height - 1)
				vStack.push_back(Pos + pTiles->m_Width);
		}
		Filled = true;
	}

	if(Filled)
	{
		std::shared_ptr<IEditorAction> pAction = std::make_shared<CEditorBrushDrawAction>(Map(), Map()->m_SelectedGroup);
		if(!pAction->IsEmpty())
			Map()->m_EditorHistory.RecordAction(pAction);
	}
}

// TClient: bounded bucket fill — the flood fill stops at tiles that are
// non-air on any boundary layer set to "Yes" (or "Only"). Clicking on a tile
// of a layer set to "Only" paints exactly the connected tiles of that layer
// on the target layers instead. The fill is applied to every selected tiles
// layer that is not a "Yes" boundary layer; with "fill boundaries" enabled
// the wall tiles touching the filled region (diagonally included) are
// painted on the target layers as well.
void CMapView::BucketFillE(float Wx, float Wy)
{
	if(Editor()->m_pBrush->IsEmpty())
		return;

	std::shared_ptr<CLayerTiles> pBrushTiles;
	for(const auto &pBrushLayer : Editor()->m_pBrush->m_vpLayers)
	{
		if(pBrushLayer->m_Type == LAYERTYPE_TILES)
		{
			pBrushTiles = std::static_pointer_cast<CLayerTiles>(pBrushLayer);
			break;
		}
	}
	if(!pBrushTiles)
		return;

	std::vector<std::shared_ptr<CLayerTiles>> vpBoundary;
	std::vector<std::shared_ptr<CLayerTiles>> vpOnly;
	const std::shared_ptr<CLayerTiles> apLayerSources[] = {
		Map()->m_pGameLayer,
		Map()->m_pFrontLayer,
		Map()->m_pTeleLayer,
		Map()->m_pTuneLayer,
		Map()->m_pSwitchLayer,
		Map()->m_pSpeedupLayer,
	};
	static_assert(std::size(apLayerSources) == CEditor::NUM_BUCKETE_BOUNDARIES);
	for(size_t b = 0; b < CEditor::NUM_BUCKETE_BOUNDARIES; b++)
	{
		const std::shared_ptr<CLayerTiles> &pLayer = apLayerSources[b];
		if(pLayer == nullptr)
			continue;
		if(Editor()->m_aBucketEBoundary[b] == CEditor::BUCKETE_STATE_YES)
			vpBoundary.push_back(pLayer);
		else if(Editor()->m_aBucketEBoundary[b] == CEditor::BUCKETE_STATE_ONLY)
			vpOnly.push_back(pLayer);
	}

	const auto IsWall = [&vpBoundary, &vpOnly](int X, int Y) {
		for(const auto &pLayer : vpBoundary)
		{
			if(X >= 0 && X < pLayer->m_Width && Y >= 0 && Y < pLayer->m_Height && pLayer->GetTile(X, Y).m_Index != 0)
				return true;
		}
		for(const auto &pLayer : vpOnly)
		{
			if(X >= 0 && X < pLayer->m_Width && Y >= 0 && Y < pLayer->m_Height && pLayer->GetTile(X, Y).m_Index != 0)
				return true;
		}
		return false;
	};
	// entity tiles that must never be painted over: tele TO/CTO/CFRM and
	// gameplay-relevant Game layer tiles (spawn, weapons, flags, doors, ...)
	const std::shared_ptr<CLayerTiles> pTeleLayer = Map()->m_pTeleLayer;
	const std::shared_ptr<CLayerTiles> pGameLayer = Map()->m_pGameLayer;
	const auto IsNeverPaintTile = [&pTeleLayer, &pGameLayer](const std::shared_ptr<CLayerTiles> &pLayer, int X, int Y) {
		if(pLayer == nullptr)
			return false;
		if(X < 0 || X >= pLayer->m_Width || Y < 0 || Y >= pLayer->m_Height)
			return false;
		const int Index = pLayer->GetTile(X, Y).m_Index;
		if(pLayer == pTeleLayer)
			return Index == 27 || Index == 29 || Index == 30;
		if(pLayer == pGameLayer)
			return (Index >= 4 && Index <= 6) || Index == 11 || Index == 13 || (Index >= 16 && Index <= 22) || (Index >= 32 && Index <= 240);
		return false;
	};
	const auto IsNeverPaint = [&](int X, int Y) {
		return IsNeverPaintTile(pTeleLayer, X, Y) || IsNeverPaintTile(pGameLayer, X, Y);
	};
	const auto IsOnlyTile = [&](int X, int Y) {
		for(const auto &pLayer : vpOnly)
		{
			if(X >= 0 && X < pLayer->m_Width && Y >= 0 && Y < pLayer->m_Height && pLayer->GetTile(X, Y).m_Index != 0)
			{
				if(IsNeverPaint(X, Y))
					continue;
				return true;
			}
		}
		return false;
	};

	const CTile Replace = pBrushTiles->GetTile(0, 0);
	bool Filled = false;
	for(size_t i = 0; i < Map()->m_vSelectedLayers.size(); i++)
	{
		std::shared_ptr<CLayer> pSelectedLayer = Map()->SelectedLayer(i);
		if(!pSelectedLayer || pSelectedLayer->m_Type != LAYERTYPE_TILES)
			continue;
		std::shared_ptr<CLayerTiles> pTiles = std::static_pointer_cast<CLayerTiles>(pSelectedLayer);
		// "Yes" boundary layers define the walls and are not filled
		if(std::find(vpBoundary.begin(), vpBoundary.end(), pTiles) != vpBoundary.end())
			continue;

		if(!vpOnly.empty())
		{
			// "Only" mode: paint exactly the tiles of the "only" layers,
			// everywhere on the map, on the target layers
			for(int Y = 0; Y < pTiles->m_Height; Y++)
			{
				for(int X = 0; X < pTiles->m_Width; X++)
				{
					if(!IsOnlyTile(X, Y))
						continue;
					pTiles->SetTile(X, Y, Replace);
					Filled = true;
				}
			}
			continue;
		}

		const int FillX = (int)Wx / 32;
		const int FillY = (int)Wy / 32;
		if(FillX < 0 || FillX >= pTiles->m_Width || FillY < 0 || FillY >= pTiles->m_Height)
			continue;

		std::vector<bool> vVisited((size_t)pTiles->m_Width * pTiles->m_Height, false);
		std::vector<int> vStack;
		vStack.push_back(FillY * pTiles->m_Width + FillX);
		while(!vStack.empty())
		{
			const int Pos = vStack.back();
			vStack.pop_back();
			if(vVisited[Pos])
				continue;
			vVisited[Pos] = true;

			const int X = Pos % pTiles->m_Width;
			const int Y = Pos / pTiles->m_Width;
			if(IsWall(X, Y))
				continue; // normal mode stops at walls

			if(X > 0)
				vStack.push_back(Pos - 1);
			if(X < pTiles->m_Width - 1)
				vStack.push_back(Pos + 1);
			if(Y > 0)
				vStack.push_back(Pos - pTiles->m_Width);
			if(Y < pTiles->m_Height - 1)
				vStack.push_back(Pos + pTiles->m_Width);
		}

		// paint the reached non-wall cells; with "fill boundaries" the wall
		// cells touching the region are painted as well — including diagonal
		// neighbours, so staircase corners are covered too
		std::vector<bool> vWallPaint((size_t)pTiles->m_Width * pTiles->m_Height, false);
		if(Editor()->m_BucketEFillBoundary != 0)
		{
			for(size_t Pos = 0; Pos < vVisited.size(); ++Pos)
			{
				if(!vVisited[Pos])
					continue;
				const int X = Pos % pTiles->m_Width;
				const int Y = Pos / pTiles->m_Width;
				if(IsWall(X, Y))
					continue;
				for(int dy = -1; dy <= 1; ++dy)
				{
					for(int dx = -1; dx <= 1; ++dx)
					{
						if(dx == 0 && dy == 0)
							continue;
						const int NX = X + dx;
						const int NY = Y + dy;
						if(NX < 0 || NX >= pTiles->m_Width || NY < 0 || NY >= pTiles->m_Height)
							continue;
						if(IsWall(NX, NY) && !IsNeverPaint(NX, NY))
							vWallPaint[(size_t)NY * pTiles->m_Width + NX] = true;
					}
				}
			}
		}

		for(size_t Pos = 0; Pos < vVisited.size(); ++Pos)
		{
			if(!vVisited[Pos])
				continue;
			const int X = Pos % pTiles->m_Width;
			const int Y = Pos / pTiles->m_Width;
			if(IsWall(X, Y))
				continue; // reached wall cells are painted via vWallPaint
			pTiles->SetTile(X, Y, Replace);
			Filled = true;
		}
		for(size_t Pos = 0; Pos < vWallPaint.size(); ++Pos)
		{
			if(!vWallPaint[Pos])
				continue;
			pTiles->SetTile(Pos % pTiles->m_Width, Pos / pTiles->m_Width, Replace);
			Filled = true;
		}
	}
	if(Filled)
	{
		std::shared_ptr<IEditorAction> pAction = std::make_shared<CEditorBrushDrawAction>(Map(), Map()->m_SelectedGroup);
		if(!pAction->IsEmpty())
			Map()->m_EditorHistory.RecordAction(pAction);
	}
}

void CMapView::UpdateMouseWorld()
{
	const vec2 UpdatedMousePos = Ui()->UpdatedMousePos();
	const vec2 UpdatedMouseDelta = Ui()->UpdatedMouseDelta();

	// fix correct world x and y
	const std::shared_ptr<CLayerGroup> pGroup = Map()->SelectedGroup();
	if(pGroup)
	{
		CScreenRect GroupRect = pGroup->Mapping();

		float WorldWidth = GroupRect.Width();
		float WorldHeight = GroupRect.Height();

		Map()->m_MapViewState.m_MouseWorldScale = WorldWidth / Graphics()->WindowWidth();

		Map()->m_MapViewState.m_MouseWorldPos.x = GroupRect.m_TopLeft.x + WorldWidth * (UpdatedMousePos.x / Graphics()->WindowWidth());
		Map()->m_MapViewState.m_MouseWorldPos.y = GroupRect.m_TopLeft.y + WorldHeight * (UpdatedMousePos.y / Graphics()->WindowHeight());
		Map()->m_MapViewState.m_MouseDeltaWorld.x = UpdatedMouseDelta.x * (WorldWidth / Graphics()->WindowWidth());
		Map()->m_MapViewState.m_MouseDeltaWorld.y = UpdatedMouseDelta.y * (WorldHeight / Graphics()->WindowHeight());
	}
	else
	{
		Map()->m_MapViewState.m_MouseWorldPos = vec2(-1.0f, -1.0f);
		Map()->m_MapViewState.m_MouseDeltaWorld = vec2(0.0f, 0.0f);
	}

	Map()->m_MapViewState.m_MouseWorldNoParaPos = vec2(-1.0f, -1.0f);
	for(const std::shared_ptr<CLayerGroup> &pGameGroup : Map()->m_vpGroups)
	{
		if(!pGameGroup->m_GameGroup)
			continue;

		CScreenRect GroupRect = pGameGroup->Mapping();

		float WorldWidth = GroupRect.Width();
		float WorldHeight = GroupRect.Height();

		Map()->m_MapViewState.m_MouseWorldNoParaPos.x = GroupRect.m_TopLeft.x + WorldWidth * (UpdatedMousePos.x / Graphics()->WindowWidth());
		Map()->m_MapViewState.m_MouseWorldNoParaPos.y = GroupRect.m_TopLeft.y + WorldHeight * (UpdatedMousePos.y / Graphics()->WindowHeight());
	}
}

void CMapView::ResetMouseDeltaWorld()
{
	Map()->m_MapViewState.m_MouseDeltaWorld = vec2(0.0f, 0.0f);
}

float CMapView::MouseWorldScale() const
{
	return Map()->m_MapViewState.m_MouseWorldScale;
}

vec2 CMapView::MouseDeltaWorld() const
{
	return Map()->m_MapViewState.m_MouseDeltaWorld;
}

vec2 CMapView::MouseWorldPos() const
{
	return Map()->m_MapViewState.m_MouseWorldPos;
}

vec2 CMapView::MouseWorldNoParaPos() const
{
	return Map()->m_MapViewState.m_MouseWorldNoParaPos;
}

void CMapView::ResetZoom()
{
	SetEditorOffset({0, 0});
	Zoom()->SetValue(100.0f);
}

float CMapView::ScaleLength(float Value) const
{
	return GetWorldZoom() * Value;
}

void CMapView::ZoomMouseTarget(float ZoomFactor)
{
	// zoom to the current mouse position
	// get absolute mouse position

	CScreenRect ScreenRect = Graphics()->MapScreenToWorld(
		GetWorldOffset().x, GetWorldOffset().y,
		100.0f, 100.0f, 100.0f, 0.0f, 0.0f, Graphics()->ScreenAspect(), GetWorldZoom());

	float WorldWidth = ScreenRect.Width();
	float WorldHeight = ScreenRect.Height();

	float MouseWorldX = ScreenRect.m_TopLeft.x + WorldWidth * (Ui()->MouseX() / Ui()->Screen()->w);
	float MouseWorldY = ScreenRect.m_TopLeft.y + WorldHeight * (Ui()->MouseY() / Ui()->Screen()->h);

	// adjust camera
	OffsetWorld((vec2(MouseWorldX, MouseWorldY) - GetWorldOffset()) * (1.0f - ZoomFactor));
}

void CMapView::UpdateZoom()
{
	float OldLevel = Zoom()->GetValue();
	bool UpdatedZoom = Zoom()->UpdateValue();
	Zoom()->SetValueRange(10.0f, g_Config.m_EdLimitMaxZoomLevel ? 2000.0f : std::numeric_limits<float>::max());
	float NewLevel = Zoom()->GetValue();
	if(UpdatedZoom && g_Config.m_EdZoomTarget)
		ZoomMouseTarget(NewLevel / OldLevel);
	Map()->m_MapViewState.m_WorldZoom = NewLevel / 100.0f;
}

CSmoothValue *CMapView::Zoom()
{
	return &Map()->m_MapViewState.m_Zoom;
}

const CSmoothValue *CMapView::Zoom() const
{
	return &Map()->m_MapViewState.m_Zoom;
}

CProofMode *CMapView::ProofMode()
{
	return &m_ProofMode;
}

const CProofMode *CMapView::ProofMode() const
{
	return &m_ProofMode;
}

CMapGrid *CMapView::MapGrid()
{
	return &m_MapGrid;
}

const CMapGrid *CMapView::MapGrid() const
{
	return &m_MapGrid;
}

void CMapView::OffsetWorld(vec2 Offset)
{
	Map()->m_MapViewState.m_WorldOffset += Offset;
}

void CMapView::OffsetEditor(vec2 Offset)
{
	Map()->m_MapViewState.m_EditorOffset += Offset;
}

void CMapView::SetWorldOffset(vec2 WorldOffset)
{
	Map()->m_MapViewState.m_WorldOffset = WorldOffset;
}

void CMapView::SetEditorOffset(vec2 EditorOffset)
{
	Map()->m_MapViewState.m_EditorOffset = EditorOffset;
}

vec2 CMapView::GetWorldOffset() const
{
	return Map()->m_MapViewState.m_WorldOffset;
}

vec2 CMapView::GetEditorOffset() const
{
	return Map()->m_MapViewState.m_EditorOffset;
}

float CMapView::GetWorldZoom() const
{
	return Map()->m_MapViewState.m_WorldZoom;
}
