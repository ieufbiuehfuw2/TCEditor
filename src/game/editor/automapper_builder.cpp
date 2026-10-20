// TClient: visual automapper builder. Opened from the "Automapper" menubar
// button. Screen 1 lists the map's 1024x1024 / 2048x2048 tilesets. Screen 2
// lets the mapper define blocks per type (1x1 up to 5x2, plus four joint
// types): click a tile in the tileset preview, click a cell of the block grid
// to place it, ctrl+click clears. Every type has up to three variant grids —
// complete variants are packed in roughly equal share. The joint types are
// corner tiles with pre-seeded neighbor conditions (two painted arms, two
// open sides). An optional global random factor is applied to every rule.
// The generated pipeline follows DDNET_AUTOMAPPER_ONESHOT.md: normalize to
// the 1x1 tile, lattice mark+convert for 2D blocks, joints, greedy run-start
// repeats for strips, 1x1 variants and marker cleanup. Saved to
// editor/automap/<tileset>.rules and the tileset's automapper is reloaded
// immediately.

#include "editor.h"

#include <base/io.h>
#include <base/log.h>
#include <base/math.h>
#include <base/str.h>

#include <engine/keys.h>
#include <engine/storage.h>

#include <game/editor/mapitems/image.h>
#include <game/editor/mapitems/layer.h>
#include <game/editor/mapitems/layer_tiles.h>

#include <algorithm>
#include <string>
#include <vector>

static const char *const s_apAmTypeNames[CEditor::NUM_AM_TYPES] = {
	"1x1",
	"1x2",
	"1x3",
	"1x4",
	"1x5",
	"2x1",
	"2x2",
	"2x3",
	"3x1",
	"3x2",
	"3x3",
	"4x1",
	"4x2",
	"5x1",
	"5x2",
	"J R+D",
	"J L+D",
	"J R+U",
	"J L+U",
};

int CEditor::AmVariantCount(int Type) const
{
	int Count = 0;
	const int Cells = s_aAmWidths[Type] * s_aAmHeights[Type];
	for(int v = 0; v < AM_VARIANTS; ++v)
	{
		bool Complete = true;
		for(int s = 0; s < Cells && Complete; ++s)
			Complete = m_aAmBlocks[Type].m_aVariants[v][s] > 0;
		if(Complete)
			Count++;
	}
	return Count;
}

bool CEditor::IsAmTypeDefined(int Type) const
{
	return AmVariantCount(Type) > 0;
}

void CEditor::OpenAutomapperBuilder()
{
	m_AmPickerView = true;
	m_AmEditType = -1;
	m_AmArmedTile = -1;
	m_AmConditionMode = false;
	m_aAmError[0] = '\0';
	m_Dialog = DIALOG_AUTOMAPPER;
}

void CEditor::CloseAutomapperBuilder()
{
	OnDialogClose();
}

// draws one tileset tile into the given rect, optionally rotated by
// Rot*90° clockwise — the same transform the game renderer applies to the
// TILEFLAG_ROTATE/XFLIP/YFLIP combos emitted in the rules
void CEditor::RenderAmTile(CUIRect Rect, const std::shared_ptr<CEditorImage> &pImage, int Index, int Rot) const
{
	if(Index < 0 || Index > 255 || pImage == nullptr)
		return;
	const int Col = Index % 16;
	const int Row = Index / 16;
	const float u0 = Col / 16.0f;
	const float v0 = Row / 16.0f;
	const float u1 = (Col + 1) / 16.0f;
	const float v1 = (Row + 1) / 16.0f;
	Graphics()->TextureSet(pImage->m_Texture);
	Graphics()->WrapClamp();
	Graphics()->QuadsBegin();
	switch(Rot & 3)
	{
	case 1: Graphics()->QuadsSetSubsetFree(u0, v1, u0, v0, u1, v0, u1, v1); break; // 90°
	case 2: Graphics()->QuadsSetSubsetFree(u1, v1, u0, v1, u1, v0, u0, v0); break; // 180°
	case 3: Graphics()->QuadsSetSubsetFree(u1, v0, u1, v1, u0, v1, u0, v0); break; // 270°
	default: Graphics()->QuadsSetSubset(u0, v0, u1, v1); break;
	}
	IGraphics::CQuadItem QuadItem(Rect.x, Rect.y, Rect.w, Rect.h);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();
	Graphics()->WrapNormal();
}

// flag tokens the automapper parser understands, encoding Rot*90° clockwise
static const char *AmRotationFlags(int Rot)
{
	switch(Rot & 3)
	{
	case 1: return " ROTATE";
	case 2: return " XFLIP YFLIP";
	case 3: return " XFLIP YFLIP ROTATE";
	}
	return "";
}

void CEditor::GenerateAutomapperRules()
{
	m_aAmError[0] = '\0';
	m_AmRulesText.clear();
	std::string &Out = m_AmRulesText;
	Out += "[Main]\n";

	// the 1x1 tile is the raw fill everything painted is normalized to
	const int Raw = m_aAmBlocks[AM_1X1].m_aVariants[0][0];
	if(Raw <= 0)
	{
		str_copy(m_aAmError, "Define the 1x1 block first: its tile is the raw fill all painted cells are normalized to.");
		Out = "# Define the 1x1 block first.\n";
		return;
	}
	const std::string RawStr = std::to_string(Raw);
	const std::string RandomLine = m_AmRandom ? "Random " + std::to_string(m_AmRandomPercent) + "%\n" : "";

	// which types are defined and how many variants each has
	int aVariantCount[NUM_AM_TYPES] = {};
	std::vector<bool> vUsed(256, false);
	vUsed[0] = true;
	vUsed[Raw] = true;
	for(int t = 0; t < NUM_AM_TYPES; ++t)
	{
		aVariantCount[t] = AmVariantCount(t);
		const int Cells = s_aAmWidths[t] * s_aAmHeights[t];
		for(int v = 0; v < AM_VARIANTS; ++v)
		{
			bool Complete = true;
			for(int s = 0; s < Cells; ++s)
				Complete = Complete && m_aAmBlocks[t].m_aVariants[v][s] > 0;
			if(Complete)
				for(int s = 0; s < Cells; ++s)
					vUsed[m_aAmBlocks[t].m_aVariants[v][s]] = true;
		}
	}

	// packing order: 2D blocks (largest area first), then joints, then strips
	// (longest first); leftovers of everything end up as the raw tile
	std::vector<int> v2D, vJoints, vStrips;
	for(int t = 0; t < NUM_AM_TYPES; ++t)
	{
		if(!IsAmTypeDefined(t) || t == AM_1X1)
			continue;
		if(s_aAmJoint[t])
			vJoints.push_back(t);
		else if(s_aAmWidths[t] > 1 && s_aAmHeights[t] > 1)
			v2D.push_back(t);
		else
			vStrips.push_back(t);
	}
	std::sort(v2D.begin(), v2D.end(), [](int a, int b) {
		const int AreaA = s_aAmWidths[a] * s_aAmHeights[a];
		const int AreaB = s_aAmWidths[b] * s_aAmHeights[b];
		if(AreaA != AreaB)
			return AreaA > AreaB;
		if(s_aAmWidths[a] != s_aAmWidths[b])
			return s_aAmWidths[a] > s_aAmWidths[b];
		return s_aAmHeights[a] > s_aAmHeights[b];
	});
	std::sort(vStrips.begin(), vStrips.end(), [](int a, int b) {
		const int LenA = std::max(s_aAmWidths[a], s_aAmHeights[a]);
		const int LenB = std::max(s_aAmWidths[b], s_aAmHeights[b]);
		if(LenA != LenB)
			return LenA > LenB;
		return s_aAmWidths[a] > s_aAmWidths[b]; // vertical before horizontal
	});

	// markers: one per (type, variant), taken from free tileset cells
	std::vector<int> vMarkers;
	for(int i = 255; i >= 192; --i)
		if(!vUsed[i])
			vMarkers.push_back(i);
	for(int i = 191; i >= 1; --i)
		if(!vUsed[i])
			vMarkers.push_back(i);
	int MarkersNeeded = 0;
	for(int t = 0; t < NUM_AM_TYPES; ++t)
		if(t != AM_1X1)
			MarkersNeeded += aVariantCount[t];
	if((int)vMarkers.size() < MarkersNeeded)
	{
		str_copy(m_aAmError, "Not enough free tileset cells to use as markers.");
		Out = "# Not enough free tileset cells to use as markers.\n";
		return;
	}
	std::vector<int> vUsedMarkers;
	auto NextMarker = [&vMarkers, &vUsedMarkers]() {
		const int Marker = vMarkers.front();
		vMarkers.erase(vMarkers.begin());
		vUsedMarkers.push_back(Marker);
		return Marker;
	};

	// condition lines shared by all variants of a type
	auto ConditionLines = [](const SAmBlock &Block, const std::string &RawTile) {
		std::string Lines;
		for(const SAmCondition &Condition : Block.m_vConditions)
		{
			if(Condition.m_Tile == AM_COND_FULL)
				Lines += "Pos " + std::to_string(Condition.m_Dx) + " " + std::to_string(Condition.m_Dy) + " INDEX " + RawTile + "\n";
			else if(Condition.m_Tile == AM_COND_EMPTY)
				Lines += "Pos " + std::to_string(Condition.m_Dx) + " " + std::to_string(Condition.m_Dy) + " NOTINDEX " + RawTile + "\n";
			else
				Lines += "Pos " + std::to_string(Condition.m_Dx) + " " + std::to_string(Condition.m_Dy) + " INDEX " + std::to_string(Condition.m_Tile) + "\n";
		}
		return Lines;
	};

	// normalize: everything painted becomes the raw fill tile
	Out += "# normalize: all painted tiles become the raw fill tile (" + RawStr + ")\n";
	Out += "Index " + RawStr + "\n";
	Out += "Pos 0 0 FULL\n";

	// 2D blocks: one mark+convert pair per lattice residue; the residue picks
	// the variant so complete variants are spread in equal share
	for(int Type : v2D)
	{
		const SAmBlock &Block = m_aAmBlocks[Type];
		const int Width = s_aAmWidths[Type];
		const int Height = s_aAmHeights[Type];
		const int Variants = aVariantCount[Type];
		const std::string Conditions = ConditionLines(Block, RawStr);
		int aMarkers[AM_VARIANTS] = {};
		for(int v = 0; v < Variants; ++v)
			aMarkers[v] = NextMarker();

		Out += "# " + std::to_string(Width) + "x" + std::to_string(Height) + " blocks (" + std::to_string(Variants) + " variants, lattice residues)\n";
		for(int k = 0; k < Width * Height; ++k)
		{
			const int ResidueX = k % Width;
			const int ResidueY = k / Width;
			const int Variant = k % Variants;
			const std::string MarkerStr = std::to_string(aMarkers[Variant]);
			Out += "NewRun\n";
			Out += "Index " + MarkerStr + "\n";
			for(int dy = 0; dy < Height; ++dy)
				for(int dx = 0; dx < Width; ++dx)
					Out += "Pos " + std::to_string(dx) + " " + std::to_string(dy) + " INDEX " + RawStr + "\n";
			Out += Conditions;
			Out += "Modulo " + std::to_string(Width) + " " + std::to_string(Height) + " " +
				std::to_string((Width - ResidueX) % Width) + " " + std::to_string((Height - ResidueY) % Height) + "\n";
			Out += RandomLine;

			Out += "NewRun\n";
			for(int s = 0; s < Width * Height; ++s)
			{
				const int sx = s % Width;
				const int sy = s / Width;
				Out += "Index " + std::to_string(Block.m_aVariants[Variant][s]) + AmRotationFlags(Block.m_aRot[Variant][s]) + "\n";
				if(s == 0)
					Out += "Pos 0 0 INDEX " + MarkerStr + "\n";
				else
				{
					Out += "Pos " + std::to_string(-sx) + " " + std::to_string(-sy) + " INDEX " + MarkerStr + "\n";
					Out += "Pos 0 0 INDEX " + RawStr + "\n";
				}
			}
		}
	}

	// joints: corner tiles with pre-seeded neighbor conditions, placed before
	// the strips so the arms pack around them
	for(int Type : vJoints)
	{
		const SAmBlock &Block = m_aAmBlocks[Type];
		const int Variants = aVariantCount[Type];
		const std::string Conditions = ConditionLines(Block, RawStr);
		int aMarkers[AM_VARIANTS] = {};
		for(int v = 0; v < Variants; ++v)
			aMarkers[v] = NextMarker();

		Out += "# joint " + std::string(s_apAmTypeNames[Type]) + "\n";
		Out += "NewRun\n";
		for(int v = 0; v < Variants; ++v)
		{
			Out += "Index " + std::to_string(aMarkers[v]) + "\n";
			Out += "Pos 0 0 INDEX " + RawStr + "\n";
			Out += Conditions;
			if(Variants > 1)
				Out += "Modulo " + std::to_string(Variants) + " 1 " + std::to_string((Variants - v) % Variants) + " 0\n";
			Out += RandomLine;
		}
		Out += "NewRun\n";
		for(int v = 0; v < Variants; ++v)
		{
			Out += "Index " + std::to_string(Block.m_aVariants[v][0]) + AmRotationFlags(Block.m_aRot[v][0]) + "\n";
			Out += "Pos 0 0 INDEX " + std::to_string(aMarkers[v]) + "\n";
		}
	}

	// strips: greedy from the run start, repeated so the whole span is
	// consumed; leftovers stay as the raw tile
	for(int Type : vStrips)
	{
		const SAmBlock &Block = m_aAmBlocks[Type];
		const int Width = s_aAmWidths[Type];
		const int Height = s_aAmHeights[Type];
		const int Length = std::max(Width, Height);
		const bool Vertical = Width == 1;
		const int Variants = aVariantCount[Type];
		const std::string Conditions = ConditionLines(Block, RawStr);
		int aMarkers[AM_VARIANTS] = {};
		for(int v = 0; v < Variants; ++v)
			aMarkers[v] = NextMarker();

		Out += "# " + std::to_string(Width) + "x" + std::to_string(Height) + " strips (" + std::to_string(Variants) + " variants, greedy run starts, " + std::to_string(m_AmPasses) + " passes)\n";
		for(int Pass = 0; Pass < m_AmPasses; ++Pass)
		{
			Out += "NewRun\n";
			for(int v = 0; v < Variants; ++v)
			{
				const std::string MarkerStr = std::to_string(aMarkers[v]);
				Out += "Index " + MarkerStr + "\n";
				Out += "Pos 0 0 INDEX " + RawStr + "\n";
				for(int d = 1; d < Length; ++d)
				{
					if(Vertical)
						Out += "Pos 0 " + std::to_string(d) + " INDEX " + RawStr + "\n";
					else
						Out += "Pos " + std::to_string(d) + " 0 INDEX " + RawStr + "\n";
				}
				if(Vertical)
					Out += "Pos 0 -1 NOTINDEX " + RawStr + "\n";
				else
					Out += "Pos -1 0 NOTINDEX " + RawStr + "\n";
				Out += Conditions;
				if(Variants > 1)
				{
					if(Vertical)
						Out += "Modulo " + std::to_string(Variants) + " 1 " + std::to_string((Variants - v) % Variants) + " 0\n";
					else
						Out += "Modulo 1 " + std::to_string(Variants) + " 0 " + std::to_string((Variants - v) % Variants) + "\n";
				}
				Out += RandomLine;
			}

			Out += "NewRun\n";
			for(int v = 0; v < Variants; ++v)
			{
				const std::string MarkerStr = std::to_string(aMarkers[v]);
				for(int s = 0; s < Length; ++s)
				{
					Out += "Index " + std::to_string(Block.m_aVariants[v][s]) + AmRotationFlags(Block.m_aRot[v][s]) + "\n";
					if(Vertical)
						Out += "Pos 0 " + std::to_string(-s) + " INDEX " + MarkerStr + "\n";
					else
						Out += "Pos " + std::to_string(-s) + " 0 INDEX " + MarkerStr + "\n";
					if(s != 0)
						Out += "Pos 0 0 INDEX " + RawStr + "\n";
				}
			}
		}
	}

	// remaining single cells become 1x1 variants by modulo (variant 0 is the
	// raw tile itself and needs no rule)
	{
		const int Extras = aVariantCount[AM_1X1] - 1;
		if(Extras > 0)
		{
			const int Total = aVariantCount[AM_1X1];
			Out += "# leftover single cells become 1x1 variants\n";
			Out += "NewRun\n";
			for(int v = 1; v <= Extras; ++v)
			{
				Out += "Index " + std::to_string(m_aAmBlocks[AM_1X1].m_aVariants[v][0]) + AmRotationFlags(m_aAmBlocks[AM_1X1].m_aRot[v][0]) + "\n";
				Out += "Pos 0 0 INDEX " + RawStr + "\n";
				Out += "Modulo " + std::to_string(Total) + " 1 " + std::to_string((Total - v) % Total) + " 0\n";
			}
		}
	}

	// safety: convert any leftover markers back to the raw tile
	if(!vUsedMarkers.empty())
	{
		Out += "# cleanup: leftover markers become raw tiles\n";
		Out += "NewRun\n";
		Out += "Index " + RawStr + "\n";
		std::string Line = "Pos 0 0 INDEX";
		for(int Marker : vUsedMarkers)
			Line += " " + std::to_string(Marker) + " OR";
		Line.resize(Line.size() - 3);
		Out += Line + "\n";
	}
}

bool CEditor::SaveAutomapperRules()
{
	if(m_pAmBuilderImage == nullptr)
	{
		str_copy(m_aAmError, "Select a tileset first.");
		return false;
	}
	if(m_aAmError[0] != '\0' || m_AmRulesText.empty())
	{
		str_copy(m_aAmError, "Fix the assignments before saving.");
		return false;
	}

	Storage()->CreateFolder("editor", IStorage::TYPE_SAVE);
	Storage()->CreateFolder("editor/automap", IStorage::TYPE_SAVE);
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "editor/automap/%s.rules", m_pAmBuilderImage->m_aName);
	IOHANDLE File = Storage()->OpenFile(aPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(File == nullptr)
	{
		str_format(m_aAmError, sizeof(m_aAmError), "Failed to write '%s'.", aPath);
		return false;
	}
	io_write(File, m_AmRulesText.data(), m_AmRulesText.size());
	io_close(File);

	// reload the automapper of the tileset so the new config is usable at once
	m_pAmBuilderImage->m_Automapper.Load(m_pAmBuilderImage->m_aName);

	str_format(m_aAmError, sizeof(m_aAmError), "Saved '%s' (%d configs). Use Auto map on a tiles layer.", aPath, m_pAmBuilderImage->m_Automapper.ConfigNamesNum());
	log_info("editor/automap", "saved %s", aPath);
	return true;
}

void CEditor::RenderAutomapperBuilder()
{
	if(m_Dialog != DIALOG_AUTOMAPPER)
		return;

	Ui()->MapScreen();
	CUIRect View = *Ui()->Screen();
	Ui()->SetHotItem(this);
	View.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.75f), IGraphics::CORNER_NONE, 0.0f);
	View.VMargin(120.0f, &View);
	View.HMargin(30.0f, &View);
	View.Draw(EditorTheme::PANEL_COLOR, IGraphics::CORNER_ALL, 5.0f);
	View.Margin(10.0f, &View);

	if(Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
	{
		CloseAutomapperBuilder();
		return;
	}

	// ---------------- screen 1: tileset picker ----------------
	if(m_AmPickerView)
	{
		CUIRect Title, Hint, List, CloseButton;
		View.HSplitTop(20.0f, &Title, &View);
		View.HSplitTop(10.0f, nullptr, &View);
		View.HSplitTop(14.0f, &Hint, &View);
		View.HSplitTop(8.0f, nullptr, &View);
		View.HSplitBottom(24.0f, &List, &CloseButton);
		Ui()->DoLabel(&Title, "Automapper builder", 12.0f, TEXTALIGN_ML);
		Ui()->DoLabel(&Hint, "Select a tileset (only 1024x1024 and 2048x2048 images are listed):", 10.0f, TEXTALIGN_ML);

		static CScrollRegion s_AmListScroll;
		CScrollRegionParams ScrollParams;
		ScrollParams.m_ScrollbarThickness = 8.0f;
		ScrollParams.m_ScrollbarMargin = 2.0f;
		ScrollParams.m_ScrollUnit = 40.0f;
		s_AmListScroll.Begin(&List, &ScrollParams);
		CUIRect RemainingList = List;
		bool AnyEntry = false;
		for(size_t i = 0; i < Map()->m_vpImages.size(); ++i)
		{
			const std::shared_ptr<CEditorImage> &pImage = Map()->m_vpImages[i];
			if(!((pImage->m_Width == 1024 && pImage->m_Height == 1024) || (pImage->m_Width == 2048 && pImage->m_Height == 2048)))
				continue;
			AnyEntry = true;
			CUIRect Entry;
			RemainingList.HSplitTop(24.0f, &Entry, &RemainingList);
			if(!s_AmListScroll.AddRect(Entry))
				continue;
			char aLabel[IO_MAX_PATH_LENGTH + 32];
			str_format(aLabel, sizeof(aLabel), "%s  (%dx%d)", pImage->m_aName, (int)pImage->m_Width, (int)pImage->m_Height);
			static int s_AmEntryIds[256] = {};
			if(i < std::size(s_AmEntryIds) && DoButton_Editor(&s_AmEntryIds[i], aLabel, 0, &Entry, BUTTONFLAG_LEFT, "Build automapper rules for this tileset."))
			{
				m_pAmBuilderImage = pImage;
				m_AmPickerView = false;
				m_AmEditType = -1;
				m_AmArmedTile = -1;
				s_AmListScroll.End();
				return;
			}
		}
		s_AmListScroll.End();
		if(!AnyEntry)
			Ui()->DoLabel(&List, "No 1024x1024 or 2048x2048 images found in this map. Add the tileset to the map first.", 10.0f, TEXTALIGN_MC);

		static int s_AmClosePicker = 0;
		if(DoButton_Editor(&s_AmClosePicker, "Close", 0, &CloseButton, BUTTONFLAG_LEFT, "Close the builder."))
		{
			CloseAutomapperBuilder();
			return;
		}
		return;
	}

	// ---------------- screen 2: rule builder ----------------
	std::shared_ptr<CEditorImage> pImage = m_pAmBuilderImage;
	GenerateAutomapperRules();

	// rotate the armed tile 90° clockwise with R
	if(m_AmArmedTile > 0 && CLineInput::GetActiveInput() == nullptr && Input()->KeyPress(KEY_R))
		m_AmArmedRot = (m_AmArmedRot + 1) % 4;

	// top bar
	CUIRect TopBar, BackButton;
	View.HSplitTop(18.0f, &TopBar, &View);
	View.HSplitTop(5.0f, nullptr, &View);
	TopBar.VSplitLeft(55.0f, &BackButton, &TopBar);
	TopBar.VSplitLeft(5.0f, nullptr, &TopBar);
	static int s_AmBackButton = 0;
	if(DoButton_Editor(&s_AmBackButton, "< Back", 0, &BackButton, BUTTONFLAG_LEFT, "Back to the tileset selection."))
	{
		m_AmPickerView = true;
		m_AmEditType = -1;
		return;
	}
	CUIRect NameLabel;
	TopBar.VSplitLeft(250.0f, &NameLabel, &TopBar);
	char aName[IO_MAX_PATH_LENGTH + 16];
	str_format(aName, sizeof(aName), "Tileset: %s", pImage != nullptr ? pImage->m_aName : "(none)");
	Ui()->DoLabel(&NameLabel, aName, 10.0f, TEXTALIGN_ML, {.m_MaxWidth = NameLabel.w, .m_EllipsisAtEnd = true});

	// random factor + passes
	CUIRect RandomButton, RandomPercent, PassesLabel, PassesValue;
	TopBar.VSplitRight(60.0f, &TopBar, nullptr);
	TopBar.VSplitRight(90.0f, &TopBar, &PassesValue);
	TopBar.VSplitRight(5.0f, &TopBar, nullptr);
	TopBar.VSplitRight(50.0f, &TopBar, &PassesLabel);
	TopBar.VSplitRight(5.0f, &TopBar, nullptr);
	TopBar.VSplitRight(55.0f, &TopBar, &RandomPercent);
	TopBar.VSplitRight(5.0f, &TopBar, nullptr);
	TopBar.VSplitRight(70.0f, &TopBar, &RandomButton);
	static int s_AmRandomButton = 0;
	if(DoButton_Editor(&s_AmRandomButton, "Random", m_AmRandom ? 1 : 0, &RandomButton, BUTTONFLAG_LEFT, "Apply a random factor to every rule so smaller blocks can take space from larger ones."))
		m_AmRandom = !m_AmRandom;
	if(m_AmRandom)
	{
		static int s_AmRandomPercentId = 0;
		m_AmRandomPercent = UiDoValueSelector(&s_AmRandomPercentId, &RandomPercent, "", m_AmRandomPercent, 5, 95, 5, 1.0f, "Random factor applied to all rules.", false, false, IGraphics::CORNER_ALL, nullptr, true).m_Value;
	}
	else
		Ui()->DoLabel(&RandomPercent, "off", 10.0f, TEXTALIGN_MC);
	Ui()->DoLabel(&PassesLabel, "Passes:", 10.0f, TEXTALIGN_MR);
	static int s_AmPassesId = 0;
	m_AmPasses = UiDoValueSelector(&s_AmPassesId, &PassesValue, "", m_AmPasses, 1, 64, 1, 1.0f, "How many greedy strip passes are generated.", false, false, IGraphics::CORNER_ALL, nullptr, true).m_Value;

	// second top row: save/close
	CUIRect TopBar2;
	View.HSplitTop(18.0f, &TopBar2, &View);
	View.HSplitTop(5.0f, nullptr, &View);
	CUIRect SaveButton, CloseButton;
	TopBar2.VSplitRight(60.0f, &TopBar2, &CloseButton);
	TopBar2.VSplitRight(5.0f, &TopBar2, nullptr);
	TopBar2.VSplitRight(110.0f, &TopBar2, &SaveButton);
	static int s_AmSaveButton = 0;
	if(DoButton_Editor(&s_AmSaveButton, "Save & reload", 0, &SaveButton, BUTTONFLAG_LEFT, "Write the rules to editor/automap/<tileset>.rules and reload the tileset's automapper."))
		SaveAutomapperRules();
	static int s_AmCloseButton = 0;
	if(DoButton_Editor(&s_AmCloseButton, "Close", 0, &CloseButton, BUTTONFLAG_LEFT, "Close the builder."))
	{
		CloseAutomapperBuilder();
		return;
	}

	// bottom: generated rules preview
	CUIRect PreviewLabel, Preview;
	View.HSplitBottom(150.0f, &View, &Preview);
	View.HSplitBottom(4.0f, &View, nullptr);
	View.HSplitBottom(12.0f, &View, &PreviewLabel);
	Ui()->DoLabel(&PreviewLabel, "Generated rules:", 10.0f, TEXTALIGN_ML);

	// ---------------- body ----------------
	CUIRect BlocksColumn, EditArea;
	View.VSplitLeft(190.0f, &BlocksColumn, &EditArea);
	EditArea.VSplitLeft(10.0f, nullptr, &EditArea);

	// block type buttons
	{
		CUIRect BlocksLabel;
		BlocksColumn.HSplitTop(14.0f, &BlocksLabel, &BlocksColumn);
		BlocksColumn.HSplitTop(4.0f, nullptr, &BlocksColumn);
		Ui()->DoLabel(&BlocksLabel, "Block types:", 10.0f, TEXTALIGN_ML);
		static int s_AmTypeButtons[NUM_AM_TYPES] = {};
		for(int t = 0; t < NUM_AM_TYPES; ++t)
		{
			CUIRect Button;
			BlocksColumn.HSplitTop(20.0f, &Button, &BlocksColumn);
			CUIRect ButtonSlot;
			const int Col = t % 3;
			for(int c = 0; c < Col; ++c)
				Button.VSplitLeft(60.0f, nullptr, &Button);
			Button.VSplitLeft(58.0f, &ButtonSlot, nullptr);
			BlocksColumn.HSplitTop(2.0f, nullptr, &BlocksColumn);

			const bool Defined = IsAmTypeDefined(t);
			const bool Partial = !Defined && std::any_of(&m_aAmBlocks[t].m_aVariants[0][0], &m_aAmBlocks[t].m_aVariants[0][0] + s_aAmWidths[t] * s_aAmHeights[t], [](int Tile) { return Tile > 0; });
			char aTooltip[160];
			if(Defined)
				str_format(aTooltip, sizeof(aTooltip), "%s: defined, %d variant(s). Click to edit.", s_apAmTypeNames[t], AmVariantCount(t));
			else if(Partial)
				str_format(aTooltip, sizeof(aTooltip), "%s: unfinished - no rules will be generated for it.", s_apAmTypeNames[t]);
			else
				str_format(aTooltip, sizeof(aTooltip), "%s: not defined. Click to define which tiles form this block.", s_apAmTypeNames[t]);
			const int Checked = m_AmEditType == t ? 1 : (Defined ? EditorButtonChecked::POSITIVE_ACTION : (Partial ? EditorButtonChecked::DANGEROUS_ACTION : 0));
			if(DoButton_Editor(&s_AmTypeButtons[t], s_apAmTypeNames[t], Checked, &ButtonSlot, BUTTONFLAG_LEFT, aTooltip))
			{
				m_AmEditType = t;
				m_AmConditionMode = false;
				m_AmArmedTile = -1;
				// seed the joint condition ring once (two painted arms, two open sides)
				if(s_aAmJoint[t] && m_aAmBlocks[t].m_vConditions.empty())
				{
					switch(t)
					{
					case AM_JOINT_RD:
						m_aAmBlocks[t].m_vConditions = {{1, 0, AM_COND_FULL}, {0, 1, AM_COND_FULL}, {-1, 0, AM_COND_EMPTY}, {0, -1, AM_COND_EMPTY}};
						break;
					case AM_JOINT_LD:
						m_aAmBlocks[t].m_vConditions = {{-1, 0, AM_COND_FULL}, {0, 1, AM_COND_FULL}, {1, 0, AM_COND_EMPTY}, {0, -1, AM_COND_EMPTY}};
						break;
					case AM_JOINT_RU:
						m_aAmBlocks[t].m_vConditions = {{1, 0, AM_COND_FULL}, {0, -1, AM_COND_FULL}, {-1, 0, AM_COND_EMPTY}, {0, 1, AM_COND_EMPTY}};
						break;
					case AM_JOINT_LU:
						m_aAmBlocks[t].m_vConditions = {{-1, 0, AM_COND_FULL}, {0, -1, AM_COND_FULL}, {1, 0, AM_COND_EMPTY}, {0, 1, AM_COND_EMPTY}};
						break;
					}
				}
			}
		}
	}

	// status line
	if(m_aAmError[0] != '\0')
	{
		CUIRect StatusRect(BlocksColumn.x, BlocksColumn.y, BlocksColumn.w, 40.0f);
		TextRender()->TextColor(m_AmRulesText.rfind('#', 0) == 0 ? ColorRGBA(1.0f, 0.6f, 0.3f, 1.0f) : ColorRGBA(0.5f, 0.9f, 0.5f, 1.0f));
		Ui()->DoLabel(&StatusRect, m_aAmError, 10.0f, TEXTALIGN_ML, {.m_MaxWidth = StatusRect.w, .m_EllipsisAtEnd = true});
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}
	else
	{
		CUIRect HintRect(BlocksColumn.x, BlocksColumn.y, BlocksColumn.w, 40.0f);
		Ui()->DoLabel(&HintRect, "Green = defined. J = joint corners with fixed conditions.", 10.0f, TEXTALIGN_ML, {.m_MaxWidth = HintRect.w, .m_EllipsisAtEnd = true});
	}

	// ---------------- block editor ----------------
	if(m_AmEditType < 0)
	{
		CUIRect HintRect = EditArea;
		Ui()->DoLabel(&HintRect, "Select a block type on the left to define its tiles.", 12.0f, TEXTALIGN_MC);
	}
	else
	{
		const int Type = m_AmEditType;
		const int Width = s_aAmWidths[Type];
		const int Height = s_aAmHeights[Type];
		SAmBlock &Block = m_aAmBlocks[Type];

		// header: block name + conditions toggle
		CUIRect Header, CondButton;
		EditArea.HSplitTop(18.0f, &Header, &EditArea);
		EditArea.HSplitTop(5.0f, nullptr, &EditArea);
		char aHeader[64];
		str_format(aHeader, sizeof(aHeader), "Block %s", s_apAmTypeNames[Type]);
		CUIRect HeaderLabel;
		Header.VSplitLeft(90.0f, &HeaderLabel, &Header);
		Ui()->DoLabel(&HeaderLabel, aHeader, 11.0f, TEXTALIGN_ML);
		Header.VSplitLeft(10.0f, nullptr, &Header);
		Header.VSplitLeft(90.0f, &CondButton, &Header);
		static int s_AmCondToggle = 0;
		if(DoButton_Editor(&s_AmCondToggle, m_AmConditionMode ? "Tiles" : "Conditions", 0, &CondButton, BUTTONFLAG_LEFT, m_AmConditionMode ? "Back to editing the block tiles." : "Edit which tiles must stand above, below, left or right of this block."))
			m_AmConditionMode = !m_AmConditionMode;

		CUIRect PreviewRect, GridsArea;
		EditArea.VSplitLeft(300.0f, &PreviewRect, &GridsArea);
		GridsArea.VSplitLeft(15.0f, nullptr, &GridsArea);

		// ---- tileset preview ----
		const float PreviewSide = std::min(PreviewRect.w, PreviewRect.h);
		CUIRect TilesetRect{PreviewRect.x, PreviewRect.y, PreviewSide, PreviewSide};
		if(pImage != nullptr)
		{
			Graphics()->TextureSet(pImage->m_Texture);
			Graphics()->WrapClamp();
			Graphics()->QuadsBegin();
			IGraphics::CQuadItem QuadItem(TilesetRect.x, TilesetRect.y, TilesetRect.w, TilesetRect.h);
			Graphics()->QuadsDrawTL(&QuadItem, 1);
			Graphics()->QuadsEnd();
			Graphics()->WrapNormal();

			Graphics()->TextureClear();
			Graphics()->LinesBegin();
			Graphics()->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.15f));
			IGraphics::CLineItem aLines[34];
			for(int i = 0; i <= 16; ++i)
			{
				const float T = i / 16.0f;
				aLines[i * 2] = IGraphics::CLineItem(TilesetRect.x + TilesetRect.w * T, TilesetRect.y, TilesetRect.x + TilesetRect.w * T, TilesetRect.y + TilesetRect.h);
				aLines[i * 2 + 1] = IGraphics::CLineItem(TilesetRect.x, TilesetRect.y + TilesetRect.h * T, TilesetRect.x + TilesetRect.w, TilesetRect.y + TilesetRect.h * T);
			}
			Graphics()->LinesDraw(aLines, 34);
			Graphics()->LinesEnd();
		}

		const vec2 Mouse(Ui()->MouseX(), Ui()->MouseY());
		int HoverTile = -1;
		if(pImage != nullptr && TilesetRect.Inside(Mouse))
		{
			const int Col = std::clamp((int)((Mouse.x - TilesetRect.x) / TilesetRect.w * 16), 0, 15);
			const int Row = std::clamp((int)((Mouse.y - TilesetRect.y) / TilesetRect.h * 16), 0, 15);
			HoverTile = Row * 16 + Col;

			Graphics()->TextureClear();
			Graphics()->QuadsBegin();
			Graphics()->SetColor(m_AmArmedTile == HoverTile ? ColorRGBA(0.95f, 0.3f, 0.3f, 0.4f) : ColorRGBA(1.0f, 1.0f, 1.0f, 0.2f));
			IGraphics::CQuadItem HoverItem(TilesetRect.x + TilesetRect.w * Col / 16.0f, TilesetRect.y + TilesetRect.h * Row / 16.0f, TilesetRect.w / 16.0f, TilesetRect.h / 16.0f);
			Graphics()->QuadsDrawTL(&HoverItem, 1);
			Graphics()->QuadsEnd();
			if(Ui()->MouseButtonClicked(0) && HoverTile > 0)
				m_AmArmedTile = HoverTile;
		}

		// ---- grids: up to three variant grids, or one condition ring ----
		const bool ConditionMode = m_AmConditionMode;
		const int GridW = ConditionMode ? Width + 2 : Width;
		const int GridH = ConditionMode ? Height + 2 : Height;
		const int GridCount = ConditionMode ? 1 : AM_VARIANTS;

		CUIRect StatusLabel;
		GridsArea.HSplitBottom(14.0f, &GridsArea, &StatusLabel);
		float Cell = std::min(std::min(GridsArea.w / (GridCount * GridW + (GridCount - 1) * 1.0f), GridsArea.h / GridH), 56.0f);
		Cell = std::max(Cell, 12.0f);

		char aStatus[200];
		if(ConditionMode)
			str_format(aStatus, sizeof(aStatus), "Conditions of %s: F = must be painted, E = must not be painted, tiles = that exact tile. Click a ring cell to set the armed tile there, Ctrl+click clears.", s_apAmTypeNames[Type]);
		else
			str_format(aStatus, sizeof(aStatus), "%s: variant 1 is required, 2-3 are optional for variety. Click a tile on the left, then a cell; R rotates the armed tile 90°. Ctrl+click clears.", s_apAmTypeNames[Type]);
		Ui()->DoLabel(&StatusLabel, aStatus, 9.0f, TEXTALIGN_MC, {.m_MaxWidth = StatusLabel.w, .m_EllipsisAtEnd = true});

		const float GridPxW = Cell * GridW;
		const float GridPxH = Cell * GridH;
		const float SlotW = (GridsArea.w - GridCount * GridPxW) / std::max(1, GridCount - 1);

		for(int g = 0; g < GridCount; ++g)
		{
			const float SlotX = GridsArea.x + g * (GridPxW + SlotW);
			CUIRect GridRect{SlotX, GridsArea.y + 14.0f, GridPxW, GridPxH};

			// variant label
			if(!ConditionMode)
			{
				CUIRect VariantLabel(GridRect.x, GridsArea.y, GridRect.w, 12.0f);
				str_format(aStatus, sizeof(aStatus), "Variant %d%s", g + 1, g == 0 ? " (required)" : " (optional)");
				Ui()->DoLabel(&VariantLabel, aStatus, 9.0f, TEXTALIGN_MC);
			}

			// cells
			for(int gy = 0; gy < GridH; ++gy)
			{
				for(int gx = 0; gx < GridW; ++gx)
				{
					CUIRect CellRect(GridRect.x + gx * Cell, GridRect.y + gy * Cell, Cell - 2.0f, Cell - 2.0f);
					const bool Inner = !ConditionMode || (gx >= 1 && gx <= Width && gy >= 1 && gy <= Height);
					if(ConditionMode && Inner)
					{
						// read-only block preview inside the ring
						CellRect.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.08f), IGraphics::CORNER_ALL, 2.0f);
						CUIRect InnerRect = CellRect;
						InnerRect.Margin(1.0f, &InnerRect);
						RenderAmTile(InnerRect, pImage, Block.m_aVariants[0][(gy - 1) * Width + (gx - 1)]);
						continue;
					}

					int CellTile = -1;
					int CellRot = 0;
					if(!ConditionMode)
					{
						const int CellIndex = gy * Width + gx;
						CellTile = Block.m_aVariants[g][CellIndex];
						CellRot = Block.m_aRot[g][CellIndex];
					}
					else
					{
						const int dx = gx - 1;
						const int dy = gy - 1;
						auto It = std::find_if(Block.m_vConditions.begin(), Block.m_vConditions.end(), [dx, dy](const SAmCondition &Condition) { return Condition.m_Dx == dx && Condition.m_Dy == dy; });
						if(It != Block.m_vConditions.end())
							CellTile = It->m_Tile;
					}

				CellRect.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.12f), IGraphics::CORNER_ALL, 2.0f);
				CUIRect InnerRect = CellRect;
				InnerRect.Margin(1.0f, &InnerRect);
				if(CellTile > 0)
					RenderAmTile(InnerRect, pImage, CellTile, CellRot);
				else if(CellTile == AM_COND_FULL)
					Ui()->DoLabel(&InnerRect, "F", 10.0f, TEXTALIGN_MC);
				else if(CellTile == AM_COND_EMPTY)
					Ui()->DoLabel(&InnerRect, "E", 10.0f, TEXTALIGN_MC);
				else if(!ConditionMode && Type == AM_1X1 && g == 0)
					Ui()->DoLabel(&InnerRect, "?", 10.0f, TEXTALIGN_MC);
				}
			}

			// grid interaction
			if(GridRect.Inside(Mouse) && Cell > 2.0f)
			{
				const int gx = std::clamp((int)((Mouse.x - GridRect.x) / Cell), 0, GridW - 1);
				const int gy = std::clamp((int)((Mouse.y - GridRect.y) / Cell), 0, GridH - 1);
				Graphics()->TextureClear();
				Graphics()->LinesBegin();
				Graphics()->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.8f));
				const float cx = GridRect.x + gx * Cell;
				const float cy = GridRect.y + gy * Cell;
				IGraphics::CLineItem aBorder[4] = {
					IGraphics::CLineItem(cx, cy, cx + Cell, cy),
					IGraphics::CLineItem(cx + Cell, cy, cx + Cell, cy + Cell),
					IGraphics::CLineItem(cx + Cell, cy + Cell, cx, cy + Cell),
					IGraphics::CLineItem(cx, cy + Cell, cx, cy)};
				Graphics()->LinesDraw(aBorder, 4);
				Graphics()->LinesEnd();

				if(Ui()->MouseButtonClicked(0))
				{
					const bool Inner = !ConditionMode || (gx >= 1 && gx <= Width && gy >= 1 && gy <= Height);
					if(!Inner)
					{
						const int dx = gx - 1;
						const int dy = gy - 1;
						auto It = std::find_if(Block.m_vConditions.begin(), Block.m_vConditions.end(), [dx, dy](const SAmCondition &Condition) { return Condition.m_Dx == dx && Condition.m_Dy == dy; });
						if(Input()->ModifierIsPressed())
						{
							if(It != Block.m_vConditions.end())
								Block.m_vConditions.erase(It);
						}
						else if(m_AmArmedTile > 0)
						{
							if(It != Block.m_vConditions.end())
								It->m_Tile = m_AmArmedTile;
							else
								Block.m_vConditions.push_back({dx, dy, m_AmArmedTile});
						}
					}
				else if(!ConditionMode)
				{
					const int s = gy * Width + gx;
					if(Input()->ModifierIsPressed())
					{
						Block.m_aVariants[g][s] = -1;
						Block.m_aRot[g][s] = 0;
					}
					else if(m_AmArmedTile > 0)
					{
						Block.m_aVariants[g][s] = m_AmArmedTile;
						Block.m_aRot[g][s] = m_AmArmedRot & 3;
					}
				}
				}
			}
		}

		// armed tile indicator
		if(m_AmArmedTile > 0)
		{
			CUIRect ArmedRect(GridsArea.x, GridsArea.y + GridsArea.h - 16.0f, 16.0f, 16.0f);
			ArmedRect.Draw(ColorRGBA(0.95f, 0.3f, 0.3f, 0.9f), IGraphics::CORNER_ALL, 2.0f);
			CUIRect InnerRect = ArmedRect;
			InnerRect.Margin(1.0f, &InnerRect);
			RenderAmTile(InnerRect, pImage, m_AmArmedTile, m_AmArmedRot & 3);
			CUIRect RotLabel(GridsArea.x + 20.0f, GridsArea.y + GridsArea.h - 16.0f, 110.0f, 16.0f);
			str_format(aStatus, sizeof(aStatus), "Armed, R: %d°", (m_AmArmedRot & 3) * 90);
			Ui()->DoLabel(&RotLabel, aStatus, 10.0f, TEXTALIGN_ML);
		}
	}

	// ---------------- rules preview ----------------
	static CScrollRegion s_AmRulesScroll;
	CScrollRegionParams RulesParams;
	RulesParams.m_ScrollbarThickness = 8.0f;
	RulesParams.m_ScrollbarMargin = 2.0f;
	RulesParams.m_ScrollUnit = 40.0f;
	s_AmRulesScroll.Begin(&Preview, &RulesParams);
	size_t Start = 0;
	while(Start <= m_AmRulesText.size())
	{
		size_t End = m_AmRulesText.find('\n', Start);
		if(End == std::string::npos)
			End = m_AmRulesText.size();
		const std::string Line = m_AmRulesText.substr(Start, End - Start);
		CUIRect LineRect;
		if(!s_AmRulesScroll.AddRect(LineRect))
			break;
		if(!Line.empty())
			Ui()->DoLabel(&LineRect, Line.c_str(), 9.0f, TEXTALIGN_ML, {.m_MaxWidth = LineRect.w, .m_EllipsisAtEnd = true});
		Start = End + 1;
	}
	s_AmRulesScroll.End();
}
