#include <algorithm>

#include <base/color.h>
#include <base/math.h>

#include <game/client/ui.h>
#include <game/editor/editor.h>
#include <game/editor/editor_ui.h>

void CEditor::UpdateTooltip(const void *pId, const CUIRect *pRect, const char *pToolTip)
{
	if(Ui()->MouseInside(pRect) && !pToolTip)
		str_copy(m_aTooltip, "");
	else if(Ui()->HotItem() == pId && pToolTip)
		str_copy(m_aTooltip, pToolTip);
}

ColorRGBA CEditor::GetButtonColor(const void *pId, int Checked)
{
	const bool Hot = Ui()->HotItem() == pId;
	if(Checked < 0)
		return ColorRGBA(1.0f, 1.0f, 1.0f, Hot ? 0.07f : 0.035f);

	switch(Checked)
	{
	case EditorButtonChecked::DANGEROUS_ACTION:
		return ColorRGBA(0.85f, 0.16f, 0.16f, Hot ? 0.85f : 0.55f);
	case EditorButtonChecked::POSITIVE_ACTION:
		return ColorRGBA(0.18f, 0.72f, 0.30f, Hot ? 0.85f : 0.55f);
	case 8: // invisible
		return ColorRGBA(0, 0, 0, 0);
	case 7: // selected + game layers
		return Hot ? EditorTheme::ACCENT_HOT.WithAlpha(0.55f) : EditorTheme::ACCENT.WithAlpha(0.35f);

	case 6: // game layers
		return ColorRGBA(1, 1, 1, Hot ? 0.14f : 0.05f);

	case 5: // selected + image/sound should be embedded
		return Hot ? EditorTheme::ACCENT_HOT.WithAlpha(0.9f) : EditorTheme::ACCENT.WithAlpha(0.65f);

	case 4: // image/sound should be embedded
		return Hot ? EditorTheme::ACCENT_HOT : EditorTheme::ACCENT.WithAlpha(0.9f);

	case 3: // selected + unused image/sound
		return Hot ? ColorRGBA(0.85f, 0.25f, 0.90f, 0.85f) : ColorRGBA(0.75f, 0.18f, 0.85f, 0.60f);

	case 2: // unused image/sound
		return Hot ? ColorRGBA(0.30f, 0.55f, 0.95f, 0.75f) : ColorRGBA(0.22f, 0.45f, 0.90f, 0.50f);

	case 1: // selected
		return Hot ? EditorTheme::ACCENT_HOT.WithAlpha(0.95f) : EditorTheme::ACCENT.WithAlpha(0.85f);

	default: // regular
		return ColorRGBA(1, 1, 1, Hot ? 0.10f : 0.045f);
	}
}

ColorRGBA CEditor::GetButtonBorderColor(const void *pId, int Checked)
{
	if(Checked == 8) // invisible
		return ColorRGBA(0, 0, 0, 0);
	if(Checked < 0) // disabled
		return ColorRGBA(1, 1, 1, 0.03f);
	return ColorRGBA(1, 1, 1, Ui()->HotItem() == pId ? 0.14f : 0.07f);
}

// Draws the button background as a rounded pill: a border rect with the fill inset by 1px
void CEditor::DrawButtonBg(const void *pId, int Checked, const CUIRect *pRect, int Corners, float Rounding, const ColorRGBA *pBgColor)
{
	pRect->Draw(GetButtonBorderColor(pId, Checked), Corners, Rounding);
	CUIRect Inner = *pRect;
	Inner.Margin(1.0f, &Inner);
	ColorRGBA Fill;
	if(pBgColor != nullptr)
	{
		// custom row color, slightly brightened on hover
		const float T = Ui()->HotItem() == pId ? 0.15f : 0.0f;
		Fill = ColorRGBA(pBgColor->r + (1.0f - pBgColor->r) * T, pBgColor->g + (1.0f - pBgColor->g) * T, pBgColor->b + (1.0f - pBgColor->b) * T, pBgColor->a);
	}
	else
		Fill = GetButtonColor(pId, Checked);
	Inner.Draw(Fill, Corners, std::max(Rounding - 1.0f, 0.0f));
}

int CEditor::DoButtonLogic(const void *pId, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip)
{
	if(Ui()->MouseInside(pRect))
	{
		if(Flags & BUTTONFLAG_RIGHT)
			m_pUiGotContext = pId;
	}

	UpdateTooltip(pId, pRect, pToolTip);
	return Ui()->DoButtonLogic(pId, Checked, pRect, Flags);
}

int CEditor::DoButton_Editor(const void *pId, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip)
{
	DrawButtonBg(pId, Checked, pRect, IGraphics::CORNER_ALL);
	CUIRect NewRect = *pRect;
	SLabelProperties Props;
	if(Checked < 0)
		Props.SetColor(ColorRGBA(0.45f, 0.45f, 0.45f, 1.0f));
	Ui()->DoLabel(&NewRect, pText, 10.0f, TEXTALIGN_MC, Props);
	Checked %= 2;
	return DoButtonLogic(pId, Checked, pRect, Flags, pToolTip);
}

int CEditor::DoButton_Env(const void *pId, const char *pText, int Checked, const CUIRect *pRect, const char *pToolTip, ColorRGBA BaseColor, int Corners)
{
	float Bright = Checked ? 1.0f : 0.5f;
	float Alpha = Ui()->HotItem() == pId ? 1.0f : 0.75f;
	ColorRGBA Color = ColorRGBA(BaseColor.r * Bright, BaseColor.g * Bright, BaseColor.b * Bright, Alpha);

	pRect->Draw(ColorRGBA(1, 1, 1, 0.10f), Corners, EditorTheme::BUTTON_ROUNDING);
	CUIRect Inner = *pRect;
	Inner.Margin(1.0f, &Inner);
	Inner.Draw(Color, Corners, EditorTheme::BUTTON_ROUNDING - 1.0f);
	Ui()->DoLabel(pRect, pText, 10.0f, TEXTALIGN_MC);
	Checked %= 2;
	return DoButtonLogic(pId, Checked, pRect, BUTTONFLAG_LEFT, pToolTip);
}

int CEditor::DoButton_Ex(const void *pId, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip, int Corners, float FontSize, int Align)
{
	DrawButtonBg(pId, Checked, pRect, Corners);

	CUIRect Rect;
	pRect->VMargin(((Align & TEXTALIGN_MASK_HORIZONTAL) == TEXTALIGN_CENTER) ? 1.0f : 5.0f, &Rect);

	SLabelProperties Props;
	Props.m_MaxWidth = Rect.w;
	Props.m_EllipsisAtEnd = true;
	if(Checked < 0)
		Props.SetColor(ColorRGBA(0.45f, 0.45f, 0.45f, 1.0f));
	Ui()->DoLabel(&Rect, pText, FontSize, Align, Props);

	return DoButtonLogic(pId, Checked, pRect, Flags, pToolTip);
}

int CEditor::DoButton_FontIcon(const void *pId, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip, int Corners, float FontSize)
{
	DrawButtonBg(pId, Checked, pRect, Corners);

	TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
	TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING);
	SLabelProperties Props;
	if(Checked < 0)
		Props.SetColor(ColorRGBA(0.45f, 0.45f, 0.45f, 1.0f));
	Ui()->DoLabel(pRect, pText, FontSize, TEXTALIGN_MC, Props);
	TextRender()->SetRenderFlags(0);
	TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);

	return DoButtonLogic(pId, Checked, pRect, Flags, pToolTip);
}

int CEditor::DoButton_MenuItem(const void *pId, const char *pText, int Checked, const CUIRect *pRect, int Flags, const char *pToolTip)
{
	if((Ui()->HotItem() == pId && Checked == 0) || Checked > 0)
		pRect->Draw(GetButtonColor(pId, Checked), IGraphics::CORNER_ALL, 4.0f);

	CUIRect Rect;
	pRect->VMargin(5.0f, &Rect);

	SLabelProperties Props;
	Props.m_MaxWidth = Rect.w;
	Props.m_EllipsisAtEnd = true;
	if(Checked < 0)
	{
		Props.SetColor(ColorRGBA(0.6f, 0.6f, 0.6f, 1.0f));
	}
	Ui()->DoLabel(&Rect, pText, 10.0f, TEXTALIGN_ML, Props);

	return DoButtonLogic(pId, Checked, pRect, Flags, pToolTip);
}

int CEditor::DoButton_DraggableEx(const void *pId, const char *pText, int Checked, const CUIRect *pRect, bool *pClicked, bool *pAbrupted, int Flags, const char *pToolTip, int Corners, float FontSize, const ColorRGBA *pBgColor)
{
	DrawButtonBg(pId, Checked, pRect, Corners, EditorTheme::BUTTON_ROUNDING, pBgColor);

	CUIRect Rect;
	pRect->VMargin(pRect->w > 20.0f ? 5.0f : 0.0f, &Rect);

	SLabelProperties Props;
	Props.m_MaxWidth = Rect.w;
	Props.m_EllipsisAtEnd = true;
	if(Checked < 0)
		Props.SetColor(ColorRGBA(0.45f, 0.45f, 0.45f, 1.0f));
	Ui()->DoLabel(&Rect, pText, FontSize, TEXTALIGN_MC, Props);

	if(Ui()->MouseInside(pRect))
	{
		if(Flags & BUTTONFLAG_RIGHT)
			m_pUiGotContext = pId;
	}

	UpdateTooltip(pId, pRect, pToolTip);
	return Ui()->DoDraggableButtonLogic(pId, Checked, pRect, pClicked, pAbrupted);
}

bool CEditor::DoEditBox(CLineInput *pLineInput, const CUIRect *pRect, float FontSize, int Corners, const char *pToolTip, const std::vector<STextColorSplit> &vColorSplits)
{
	UpdateTooltip(pLineInput, pRect, pToolTip);
	return Ui()->DoEditBox(pLineInput, pRect, FontSize, Corners, vColorSplits);
}

bool CEditor::DoClearableEditBox(CLineInput *pLineInput, const CUIRect *pRect, float FontSize, int Corners, const char *pToolTip, const std::vector<STextColorSplit> &vColorSplits)
{
	UpdateTooltip(pLineInput, pRect, pToolTip);
	return Ui()->DoClearableEditBox(pLineInput, pRect, FontSize, Corners, vColorSplits);
}

SEditResult<int> CEditor::UiDoValueSelector(const void *pId, CUIRect *pRect, const char *pLabel, int Current, int Min, int Max, int Step, float Scale, const char *pToolTip, bool IsDegree, bool IsHex, int Corners, const ColorRGBA *pColor, bool ShowValue)
{
	// logic
	static bool s_DidScroll = false;
	static float s_ScrollValue = 0.0f;
	static CLineInputNumber s_NumberInput;
	static int s_ButtonUsed = -1;
	static const void *s_pLastTextId = nullptr;

	const bool Inside = Ui()->MouseInside(pRect);
	const int Base = IsHex ? 16 : 10;

	if(Ui()->HotItem() == pId && s_ButtonUsed >= 0 && !Ui()->MouseButton(s_ButtonUsed))
	{
		Ui()->DisableMouseLock();
		if(Ui()->CheckActiveItem(pId))
		{
			Ui()->SetActiveItem(nullptr);
		}
		if(Inside && ((s_ButtonUsed == 0 && !s_DidScroll && Ui()->DoDoubleClickLogic(pId)) || s_ButtonUsed == 1))
		{
			s_pLastTextId = pId;
			s_NumberInput.SetInteger(Current, Base);
			s_NumberInput.SelectAll();
		}
		s_ButtonUsed = -1;
	}

	if(s_pLastTextId == pId)
	{
		str_copy(m_aTooltip, "Type your number. Press enter to confirm.");
		Ui()->SetActiveItem(&s_NumberInput);
		DoEditBox(&s_NumberInput, pRect, 10.0f, Corners);

		if(Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER) || ((Ui()->MouseButtonClicked(1) || Ui()->MouseButtonClicked(0)) && !Inside))
		{
			Current = std::clamp(s_NumberInput.GetInteger(Base), Min, Max);
			Ui()->DisableMouseLock();
			Ui()->SetActiveItem(nullptr);
			s_pLastTextId = nullptr;
		}

		if(Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
		{
			Ui()->DisableMouseLock();
			Ui()->SetActiveItem(nullptr);
			s_pLastTextId = nullptr;
		}
	}
	else
	{
		if(Ui()->CheckActiveItem(pId))
		{
			if(s_ButtonUsed == 0 && Ui()->MouseButton(0))
			{
				s_ScrollValue += Ui()->MouseDeltaX() * (Input()->ShiftIsPressed() ? 0.05f : 1.0f);

				if(absolute(s_ScrollValue) >= Scale)
				{
					int Count = (int)(s_ScrollValue / Scale);
					s_ScrollValue = std::fmod(s_ScrollValue, Scale);
					Current += Step * Count;
					Current = std::clamp(Current, Min, Max);
					s_DidScroll = true;

					// Constrain to discrete steps
					if(Count > 0)
						Current = Current / Step * Step;
					else
						Current = std::ceil(Current / (float)Step) * Step;
				}
			}

			if(pToolTip && s_pLastTextId != pId)
				str_copy(m_aTooltip, pToolTip);
		}
		else if(Ui()->HotItem() == pId)
		{
			if(Ui()->MouseButton(0))
			{
				s_ButtonUsed = 0;
				s_DidScroll = false;
				s_ScrollValue = 0.0f;
				Ui()->SetActiveItem(pId);
				Ui()->EnableMouseLock(pId);
			}
			else if(Ui()->MouseButton(1))
			{
				s_ButtonUsed = 1;
				Ui()->SetActiveItem(pId);
			}

			if(pToolTip && s_pLastTextId != pId)
				str_copy(m_aTooltip, pToolTip);
		}

		// render
		char aBuf[128];
		if(pLabel[0] != '\0')
		{
			if(ShowValue)
				str_format(aBuf, sizeof(aBuf), "%s %d", pLabel, Current);
			else
				str_copy(aBuf, pLabel);
		}
		else if(IsDegree)
			str_format(aBuf, sizeof(aBuf), "%d°", Current);
		else if(IsHex)
			str_format(aBuf, sizeof(aBuf), "#%06X", Current);
		else
			str_format(aBuf, sizeof(aBuf), "%d", Current);
		if(pColor)
		{
			pRect->Draw(ColorRGBA(1, 1, 1, 0.10f), Corners, EditorTheme::BUTTON_ROUNDING);
			CUIRect Inner = *pRect;
			Inner.Margin(1.0f, &Inner);
			Inner.Draw(*pColor, Corners, EditorTheme::BUTTON_ROUNDING - 1.0f);
		}
		else
			DrawButtonBg(pId, 0, pRect, Corners);
		Ui()->DoLabel(pRect, aBuf, 10, TEXTALIGN_MC);
	}

	if(Inside && !Ui()->MouseButton(0) && !Ui()->MouseButton(1))
		Ui()->SetHotItem(pId);

	static const void *s_pEditing = nullptr;
	EEditState State = EEditState::NONE;
	if(s_pEditing == pId)
	{
		State = EEditState::EDITING;
	}
	if(((Ui()->CheckActiveItem(pId) && Ui()->CheckMouseLock() && s_DidScroll) || s_pLastTextId == pId) && s_pEditing != pId)
	{
		State = EEditState::START;
		s_pEditing = pId;
	}
	if(!Ui()->CheckMouseLock() && s_pLastTextId != pId && s_pEditing == pId)
	{
		State = EEditState::END;
		s_pEditing = nullptr;
	}

	return SEditResult<int>{State, Current};
}

void CEditor::RenderBackground(CUIRect View, IGraphics::CTextureHandle Texture, float Size, float Brightness) const
{
	Graphics()->TextureSet(Texture);
	Graphics()->QuadsBegin();
	Graphics()->SetColor(Brightness, Brightness, Brightness, 1.0f);
	Graphics()->QuadsSetSubset(0, 0, View.w / Size, View.h / Size);
	IGraphics::CQuadItem QuadItem(View.x, View.y, View.w, View.h);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();
}
