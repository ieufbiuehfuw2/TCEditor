#ifndef GAME_EDITOR_EDITOR_UI_H
#define GAME_EDITOR_EDITOR_UI_H

#include <iterator>

#include <game/client/ui_listbox.h>

struct SEditBoxDropdownContext
{
	bool m_Visible = false;
	int m_Selected = -1;
	CListBox m_ListBox;
	bool m_ShortcutUsed = false;
	bool m_DidBecomeVisible = false;
	bool m_MousePressedInside = false;
	bool m_ShouldHide = false;
	int m_Width = 0;
};

class CEditorUiElements
{
public:
	CScrollRegion m_LayersScrollRegion;
	CScrollRegion m_ImagesScrollRegion;
	CScrollRegion m_SoundsScrollRegion;
};

// TODO: add and use constants for other special Checked-values in CEditor::GetButtonColor
namespace EditorButtonChecked
{
	inline constexpr int DANGEROUS_ACTION = 9;
	inline constexpr int POSITIVE_ACTION = 10;
}

// Dark theme palette for the editor UI (TClient)
namespace EditorTheme
{
	// Buttons are drawn as rounded pills: a border rect with the fill inset by 1px
	inline constexpr float BUTTON_ROUNDING = 6.0f;

	// Panels
	inline constexpr ColorRGBA MENUBAR_COLOR = ColorRGBA(0.039f, 0.039f, 0.047f, 1.0f);
	inline constexpr ColorRGBA PANEL_COLOR = ColorRGBA(0.055f, 0.055f, 0.063f, 1.0f);

	// Brightness multiplier applied to the light checker texture behind the map
	inline constexpr float CHECKER_BRIGHTNESS = 0.25f;

	// Accent color for selected/active elements
	inline constexpr ColorRGBA ACCENT = ColorRGBA(0.757f, 0.094f, 0.184f, 1.0f);
	inline constexpr ColorRGBA ACCENT_HOT = ColorRGBA(0.859f, 0.157f, 0.255f, 1.0f);

	// White glow drawn around rows matched by the layer search
	inline constexpr ColorRGBA SEARCH_GLOW_COLOR = ColorRGBA(1.0f, 1.0f, 1.0f, 0.90f);

	// Group color palette, cycled with Shift+left click on a group row
	// (red, light green, light blue, pink, yellow, light purple, orange, blue)
	inline constexpr ColorRGBA GROUP_COLORS[] = {
		ColorRGBA(0.937f, 0.204f, 0.282f, 1.0f), // red
		ColorRGBA(0.451f, 0.898f, 0.404f, 1.0f), // light green
		ColorRGBA(0.322f, 0.741f, 0.988f, 1.0f), // light blue
		ColorRGBA(0.988f, 0.478f, 0.749f, 1.0f), // pink
		ColorRGBA(0.988f, 0.859f, 0.322f, 1.0f), // yellow
		ColorRGBA(0.686f, 0.522f, 0.988f, 1.0f), // light purple
		ColorRGBA(0.988f, 0.596f, 0.204f, 1.0f), // orange
		ColorRGBA(0.204f, 0.369f, 0.937f, 1.0f), // blue
	};
	inline constexpr int NUM_GROUP_COLORS = (int)std::size(GROUP_COLORS);
}

// Edit box background function matching the dark editor theme
class CEditorEditBoxColorFunction : public IButtonColorFunction
{
public:
	ColorRGBA GetColor(bool Active, bool Hovered) const override
	{
		if(Active)
			return ColorRGBA(1.0f, 1.0f, 1.0f, 0.10f);
		if(Hovered)
			return ColorRGBA(1.0f, 1.0f, 1.0f, 0.075f);
		return ColorRGBA(1.0f, 1.0f, 1.0f, 0.055f);
	}
};

namespace EditorFontSizes
{
	[[maybe_unused]] static constexpr float MENU = 10.0f;
}

#endif
