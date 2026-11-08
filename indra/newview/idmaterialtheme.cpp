/**
 * @file idmaterialtheme.cpp
 * @brief <ID:MaterialTheme> Drives the UI colour table from the Material You palette while the
 * idyll_material skin is active.
 *
 * Part of Five's custom Firestorm fork.
 */

#include "llviewerprecompiledheaders.h"
#include "idmaterialtheme.h"

#include "idmaterialpalette.h"
#include "lldir.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"
#include "v3color.h"

namespace
{
    using idmaterial::Role;

    struct ChromeColor
    {
        const char* name;
        Role role;
        float alpha;
    };

    const ChromeColor CHROME_COLORS[] = {
        { "AccentColor", Role::Primary, 1.f },
        { "EmphasisColor", Role::Primary, 1.f },
        { "EmphasisColorLight", Role::PrimaryContainer, 1.f },
        { "EmphasisColor_13", Role::Primary, 0.13f },
        { "EmphasisColor_35", Role::Primary, 0.35f },
        { "SchemeLightest", Role::Primary, 1.f },
        { "SchemeLightest_35", Role::Primary, 0.35f },
        { "SchemeDark", Role::PrimaryContainer, 1.f },
        { "FocusColor", Role::Primary, 1.f },
        { "SelectionColor", Role::PrimaryContainer, 1.f },
        { "ChicletFlashColor", Role::Primary, 1.f },
        { "HighlightPanelBackground", Role::SecondaryContainer, 1.f },
        { "ToolbarDropZoneColor", Role::Primary, 0.5f },

        { "FloaterDefaultBackgroundColor", Role::SurfaceContainer, 0.9f },
        { "FloaterFocusBackgroundColor", Role::SurfaceContainer, 1.f },
        { "FloaterFocusBorderColor", Role::OutlineVariant, 1.f },
        { "FloaterUnfocusBorderColor", Role::OutlineVariant, 0.5f },
        { "FloaterButtonImageColor", Role::OnSurfaceVariant, 1.f },
        { "TitleBarFocusColor", Role::OnSurface, 0.08f },
        { "PanelDefaultBackgroundColor", Role::SurfaceContainerLow, 1.f },
        { "PanelFocusBackgroundColor", Role::SurfaceContainerHigh, 1.f },
        { "PanelDefaultHighlightLight", Role::OutlineVariant, 1.f },
        { "PanelGray", Role::SurfaceContainerHigh, 1.f },
        { "MouseGray", Role::SurfaceContainerHigh, 1.f },
        { "PanelDark", Role::SurfaceContainerLowest, 1.f },
        { "PanelNotificationListItem", Role::SurfaceContainerHighest, 0.5f },
        { "BottomTrayOpaqueColor", Role::SurfaceContainer, 1.f },
        { "BackgroundChatColor", Role::SurfaceContainer, 1.f },
        { "ChatHistoryBgColor", Role::SurfaceContainerLow, 0.66f },
        { "ChatHeaderAlphaBackground", Role::SurfaceContainer, 0.66f },
        { "ChatHeaderOpaqueBackground", Role::SurfaceContainer, 1.f },
        { "UploadDialogBackground", Role::SurfaceContainer, 1.f },
        { "BuildToolsTextBackgroundColor", Role::SurfaceContainerHigh, 1.f },
        { "ColorDropShadow", Role::Shadow, 0.5f },
        { "DefaultHighlightDark", Role::OutlineVariant, 1.f },
        { "DefaultHighlightLight", Role::OutlineVariant, 1.f },
        { "DefaultShadowDark", Role::OutlineVariant, 1.f },
        { "DefaultShadowLight", Role::OutlineVariant, 1.f },
        { "ResizebarBorderLight", Role::OutlineVariant, 1.f },
        { "ResizebarBorderDark", Role::OutlineVariant, 1.f },
        { "ResizebarBody", Role::SurfaceContainerHigh, 1.f },
        { "ColorSwatchBorderColor", Role::Outline, 1.f },
        { "ColorSwatchBorderColorGray", Role::OutlineVariant, 1.f },

        { "LabelTextColor", Role::OnSurface, 1.f },
        { "LabelSelectedColor", Role::OnSurface, 1.f },
        { "LabelDisabledColor", Role::OnSurface, 0.38f },
        { "LabelSelectedDisabledColor", Role::OnSurface, 0.38f },
        { "AccordionHeaderTextColor", Role::OnSurface, 1.f },
        { "TimeTextColor", Role::OnSurface, 1.f },
        { "HealthTextColor", Role::OnSurface, 1.f },
        { "CurrencyColor", Role::OnSurface, 1.f },
        { "FpsDisplayUncappedColor", Role::OnSurface, 1.f },
        { "FpsDisplayUnfocussedColor", Role::OnSurfaceVariant, 1.f },
        { "MediaListItemColor", Role::OnSurface, 1.f },
        { "GroupVisibleInProfile", Role::OnSurface, 1.f },
        { "GroupHiddenInProfile", Role::OnSurfaceVariant, 1.f },
        { "HTMLLinkColor", Role::Primary, 1.f },
        { "IconEnabledColor", Role::OnSurfaceVariant, 1.f },
        { "IconDisabledColor", Role::OnSurface, 0.38f },

        { "TextFgColor", Role::OnSurface, 1.f },
        { "TextDefaultColor", Role::OnSurface, 1.f },
        { "TextFgReadOnlyColor", Role::OnSurfaceVariant, 1.f },
        { "TextFgTentativeColor", Role::OnSurfaceVariant, 1.f },
        { "White_50", Role::OnSurfaceVariant, 1.f },
        { "LtGray_50", Role::OnSurfaceVariant, 1.f },
        { "White_25", Role::OnSurface, 0.38f },
        { "TextCursorColor", Role::Primary, 1.f },
        { "TextBgFocusColor", Role::SurfaceContainerHighest, 1.f },
        { "TextBgWriteableColor", Role::SurfaceContainerHighest, 1.f },
        { "TextBgReadOnlyColor", Role::SurfaceContainerLow, 1.f },
        { "TextBgHighlightColor", Role::PrimaryContainer, 1.f },
        { "TextEmbeddedItemColor", Role::Tertiary, 1.f },
        { "TextEmbeddedItemReadOnlyColor", Role::Tertiary, 1.f },
        { "FilterBackgroundColor", Role::SurfaceContainerHighest, 1.f },
        { "FilterTextColor", Role::Primary, 1.f },
        { "NotecardBackgroundColor", Role::SurfaceContainerHighest, 1.f },
        { "NotecardTextColor", Role::OnSurface, 1.f },
        { "NotecardSelectionColor", Role::PrimaryContainer, 1.f },
        { "NotecardCursorColor", Role::Primary, 1.f },
        { "SearchableControlHighlightFontColor", Role::Primary, 1.f },
        { "SearchableControlHighlightBgColor", Role::PrimaryContainer, 1.f },

        { "ButtonImageColor", Role::SecondaryContainer, 1.f },
        { "ButtonColor", Role::Primary, 1.f },
        { "ButtonCautionImageColor", Role::Error, 1.f },
        { "ButtonLabelColor", Role::OnSecondaryContainer, 1.f },
        { "ButtonLabelSelectedColor", Role::OnSecondaryContainer, 1.f },
        { "ButtonLabelDisabledColor", Role::OnSurface, 0.38f },
        { "ButtonLabelSelectedDisabledColor", Role::OnSurface, 0.38f },
        { "ButtonSelectedBgColor", Role::PrimaryContainer, 1.f },
        { "ButtonSelectedColor", Role::OnPrimaryContainer, 1.f },
        { "ButtonFlashBgColor", Role::InversePrimary, 1.f },

        { "ComboListBgColor", Role::SurfaceContainerHigh, 1.f },
        { "ScrollBgWriteableColor", Role::SurfaceContainerLow, 1.f },
        { "ScrollBGStripeColor", Role::OnSurface, 0.03f },
        { "ScrollDisabledColor", Role::OnSurface, 0.38f },
        { "ScrollHoveredColor", Role::OnSurface, 0.08f },
        { "ScrollHighlightedColor", Role::OnSurface, 0.12f },
        { "ScrollSelectedBGColor", Role::SecondaryContainer, 1.f },
        { "ScrollSelectedFGColor", Role::OnSecondaryContainer, 1.f },
        { "ScrollUnselectedColor", Role::OnSurface, 1.f },
        { "SelectedBgReadOnlyColor", Role::SecondaryContainer, 0.6f },
        { "ScrollbarThumbColor", Role::Outline, 1.f },
        { "ScrollbarTrackColor", Role::SurfaceContainerHighest, 1.f },

        { "SliderThumbCenterColor", Role::Primary, 1.f },
        { "SliderThumbOutlineColor", Role::Primary, 1.f },
        { "SliderDisabledThumbColor", Role::OnSurface, 0.38f },
        { "SliderTrackColor", Role::SurfaceContainerHighest, 1.f },
        { "MultiSliderThumbCenterColor", Role::Primary, 1.f },
        { "MultiSliderThumbCenterSelectedColor", Role::Tertiary, 1.f },
        { "MultiSliderThumbOutlineColor", Role::Primary, 1.f },
        { "MultiSliderDisabledThumbColor", Role::OnSurface, 0.38f },
        { "MultiSliderTrackColor", Role::SurfaceContainerHighest, 1.f },
        { "MultiSliderTriangleColor", Role::Tertiary, 1.f },
        { "Slidermark", Role::Outline, 1.f },

        { "MenuBarBgColor", Role::SurfaceContainer, 1.f },
        { "MenuDefaultBgColor", Role::SurfaceContainerHigh, 1.f },
        { "MenuPopupBgColor", Role::SurfaceContainerHigh, 1.f },
        { "MenuItemEnabledColor", Role::OnSurface, 1.f },
        { "MenuItemDisabledColor", Role::OnSurface, 0.38f },
        { "MenuItemHighlightBgColor", Role::SecondaryContainer, 1.f },
        { "MenuItemHighlightFgColor", Role::OnSecondaryContainer, 1.f },
        { "MenuItemFlashBgColor", Role::PrimaryContainer, 1.f },
        { "PieMenuBgColor", Role::SurfaceContainerHigh, 0.8f },
        { "PieMenuLineColor", Role::OutlineVariant, 0.5f },
        { "PieMenuTextColor", Role::OnSurface, 1.f },
        { "PieMenuSelectedColor", Role::SecondaryContainer, 0.6f },

        { "ToolTipBgColor", Role::InverseSurface, 1.f },
        { "ToolTipBorderColor", Role::InverseSurface, 1.f },
        { "ToolTipTextColor", Role::InverseOnSurface, 1.f },
        { "HTMLLinkColorHovertips", Role::InversePrimary, 1.f },
        { "InspectorTipTextColor", Role::OnSurface, 1.f },
        { "HintBg", Role::SurfaceContainerHighest, 1.f },
        { "HintTitle", Role::Primary, 1.f },
        { "HintText", Role::OnSurface, 1.f },

        { "AlertBoxColor", Role::SurfaceContainerHigh, 1.f },
        { "AlertTextColor", Role::OnSurface, 1.f },
        { "AlertCautionBoxColor", Role::ErrorContainer, 1.f },
        { "AlertCautionTextColor", Role::OnErrorContainer, 1.f },
        { "NotifyBoxColor", Role::SurfaceContainerHigh, 1.f },
        { "NotifyTextColor", Role::OnSurface, 1.f },
        { "NotifyCautionBoxColor", Role::ErrorContainer, 1.f },
        { "NotifyCautionWarnColor", Role::OnErrorContainer, 1.f },
        { "GroupNotifyBoxColor", Role::SecondaryContainer, 1.f },
        { "GroupNotifyTextColor", Role::OnSecondaryContainer, 1.f },
        { "GroupNotifyDimmedTextColor", Role::OnSurfaceVariant, 1.f },
        { "ScriptDialog", Role::TertiaryContainer, 1.f },
        { "ScriptDialogFg", Role::OnTertiaryContainer, 1.f },
        { "SysWellItemSelected", Role::SecondaryContainer, 1.f },
        { "BadgeImageColor", Role::Error, 1.f },
        { "BadgeBorderColor", Role::Surface, 1.f },
        { "BadgeLabelColor", Role::OnError, 1.f },
        { "LoginProgressBoxCenterColor", Role::SurfaceContainer, 0.78f },
        { "LoginProgressBoxTextColor", Role::OnSurface, 1.f },

        { "InventoryItemEnabledColor", Role::OnSurface, 1.f },
        { "InventoryItemSuffixColor", Role::OnSurfaceVariant, 1.f },
        { "InventoryItemLinkColor", Role::OnSurfaceVariant, 1.f },
        { "InventoryItemLibraryColor", Role::OnSurfaceVariant, 1.f },
        { "InventoryItemHighlightBgColor", Role::SecondaryContainer, 1.f },
        { "InventoryItemHighlightFgColor", Role::OnSecondaryContainer, 1.f },
        { "InventoryFocusOutlineColor", Role::Outline, 1.f },
        { "InventoryMouseOverColor", Role::OnSurface, 0.08f },
        { "InventorySearchStatusColor", Role::Primary, 1.f },
        { "InventoryProtectedColor", Role::Tertiary, 1.f },
        { "FolderViewLoadingMessageTextColor", Role::Primary, 1.f },
        { "SelectedOutfitTextColor", Role::Primary, 1.f },
        { "WornOutfitTextColor", Role::Tertiary, 1.f },
        { "OutfitGalleryItemSelected", Role::Primary, 1.f },
        { "OutfitGalleryItemWorn", Role::Secondary, 1.f },
        { "OutfitGalleryItemUnselected", Role::OutlineVariant, 1.f },
        { "ChatTimestampColor", Role::OnSurfaceVariant, 1.f },
        { "ChatTeleportSeparatorColor", Role::OutlineVariant, 1.f },
    };

    struct FunctionalColor
    {
        const char* name;
        uint32_t light;
        uint32_t dark;
        float alpha;
    };

    const FunctionalColor FUNCTIONAL_COLORS[] = {
        { "AgentChatColor", 0x1A1C1C, 0xF2F2F2, 1.f },
        { "AgentIMColor", 0x1A1C1C, 0xF2F2F2, 1.f },
        { "UserChatColor", 0x6B5800, 0xFFF59D, 1.f },
        { "llOwnerSayChatColor", 0x7A4F00, 0xFFE9A8, 1.f },
        { "FriendsChatColor", 0x111111, 0xFFFFFF, 1.f },
        { "LindenChatColor", 0x0B57D0, 0x8EB8FF, 1.f },
        { "MutedChatColor", 0x555A5F, 0xB8B8B8, 1.f },
        { "ObjectChatColor", 0x8A4B1F, 0xE3B48F, 1.f },
        { "ObjectIMColor", 0x8A4B1F, 0xE3B48F, 1.f },
        { "DirectChatColor", 0x9A4A00, 0xFFCC99, 1.f },
        { "SystemChatColor", 0x44474A, 0xC4C7C5, 1.f },
        { "ChatHistoryTextColor", 0x1F2322, 0xE0E3E2, 1.f },
        { "ChatHistoryMessageFromLog", 0x555A5F, 0xB8B8B8, 1.f },
        { "ChatHistoryMessageFromServerLog", 0x2E6B3A, 0x8FC79A, 1.f },
        { "ChatMentionFont", 0x3D6B00, 0xB5E86A, 1.f },
        { "ChatMentionHighlight", 0xFFD8C2, 0x5C3B26, 1.f },
        { "ChatSelfMentionHighlight", 0xFFDAD4, 0x6B2018, 1.f },
        { "MentionFlashBgColor", 0xFFE17B, 0x554500, 1.f },

        { "ChatNameColor", 0x3B6A00, 0xC0EB7D, 1.f },
        { "ChatNameObjectColor", 0x3B6A00, 0xC0EB7D, 1.f },
        { "ChatToastAgentNameColor", 0x8A4B1F, 0xE3B48F, 1.f },
        { "AgentLinkColor", 0x111111, 0xFFFFFF, 1.f },
        { "GroupLinkColor", 0x111111, 0xFFFFFF, 1.f },
        { "ConversationFriendColor", 0x006C4F, 0x6BD9B5, 1.f },

        { "StatusUserOnline", 0x146C2E, 0x6DD58C, 1.f },
        { "StatusUserOffline", 0xB3261E, 0xFF9E96, 1.f },
        { "StatusUserUnknown", 0x444746, 0xE0E0E0, 1.f },
        { "AvatarListItemIconDefaultColor", 0xFFFFFF, 0xFFFFFF, 1.f },
        { "AvatarListItemIconOnlineColor", 0xFFFFFF, 0xFFFFFF, 1.f },
        { "AvatarListItemIconOfflineColor", 0x808080, 0x808080, 0.5f },
        { "AvatarListItemIconVoiceInvitedColor", 0x808080, 0x808080, 0.5f },
        { "AvatarListItemIconVoiceJoinedColor", 0xFFFFFF, 0xFFFFFF, 1.f },
        { "AvatarListItemIconVoiceLeftColor", 0x808080, 0x808080, 0.5f },

        { "AvatarListItemChatRange", 0x3B6A00, 0xC0EB7D, 1.f },
        { "AvatarListItemShoutRange", 0x1F2322, 0xE0E3E2, 1.f },
        { "AvatarListItemBeyondShoutRange", 0x555A5F, 0xB8B8B8, 1.f },
        { "AvatarListItemAgeAlert", 0xB3261E, 0xFF9E96, 1.f },

        { "NetMapBackgroundColor", 0x000000, 0x000000, 0.25f },
        { "NetMapGroupOwnAboveWater", 0xFF00FF, 0xFF00FF, 1.f },
        { "NetMapGroupOwnBelowWater", 0xC700C7, 0xC700C7, 1.f },
        { "NetMapOtherOwnAboveWater", 0x3D3D3D, 0x3D3D3D, 1.f },
        { "NetMapOtherOwnBelowWater", 0x202020, 0x202020, 1.f },
        { "NetMapYouOwnAboveWater", 0x00FFFF, 0x00FFFF, 1.f },
        { "NetMapYouOwnBelowWater", 0x00C7C7, 0x00C7C7, 1.f },
        { "MapAvatarColor", 0xBA001F, 0xBA001F, 1.f },
        { "MapAvatarSelfColor", 0xFFFF00, 0xFFFF00, 1.f },
        { "MapAvatarFriendColor", 0x00FF00, 0x00FF00, 1.f },
        { "MapAvatarLindenColor", 0x0000FF, 0x0000FF, 1.f },
        { "MapAvatarMutedColor", 0x666666, 0x666666, 1.f },
        { "MapFrustumColor", 0xFFFFFF, 0xFFFFFF, 0.1f },
        { "MapPickRadiusColor", 0xFFFFFF, 0xFFFFFF, 0.1f },
        { "MapTrackColor", 0xBA001F, 0xBA001F, 1.f },
        { "MapTrackColorUnder", 0x0000FF, 0x0000FF, 1.f },
        { "MapTrackDisabledColor", 0x990000, 0x990000, 1.f },
        { "MapWhisperRingColor", 0x0000FF, 0x0000FF, 0.3f },
        { "MapChatRingColor", 0xFFFF00, 0xFFFF00, 0.3f },
        { "MapShoutRingColor", 0xFF0000, 0xFF0000, 0.3f },
        { "MapParcelOutlineColor", 0xFFFFFF, 0xFFFFFF, 1.f },

        { "ScriptErrorColor", 0xB3261E, 0xFF9E96, 1.f },
    };

    const char* const WORLD_CHAT_COLORS[] = {
        "AgentChatColor", "AgentIMColor", "UserChatColor", "llOwnerSayChatColor", "FriendsChatColor", "LindenChatColor",
        "MutedChatColor", "ObjectChatColor", "ObjectIMColor", "DirectChatColor", "SystemChatColor", "ScriptErrorColor",
    };

    LLColor4 functionalColor(const FunctionalColor& c, bool dark)
    {
        const uint32_t rgb = dark ? c.dark : c.light;
        return LLColor4(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, c.alpha);
    }

    const FunctionalColor* findFunctional(std::string_view name)
    {
        for (const FunctionalColor& c : FUNCTIONAL_COLORS)
            if (name == c.name)
                return &c;
        return nullptr;
    }
}

bool IDMaterialTheme::isActive()
{
    return gDirUtilp->getSkinFolder() == SKIN_FOLDER;
}

const std::vector<std::string>& IDMaterialTheme::chromeColorNames()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const ChromeColor& c : CHROME_COLORS)
            out.emplace_back(c.name);
        return out;
    }();
    return names;
}

void IDMaterialTheme::initOnStartup()
{
    if (!isActive())
        return;

    apply();
    for (const char* name : { "IDMaterialDarkMode", "IDMaterialAccent" })
    {
        if (LLControlVariable* control = gSavedSettings.getControl(name))
        {
            mSettingConnections.emplace_back(control->getSignal()->connect(
                [this](LLControlVariable*, const LLSD&, const LLSD&) { apply(); }));
        }
    }
}

void IDMaterialTheme::apply()
{
    if (!isActive())
        return;

    const LLColor3 accent = gSavedSettings.getColor3("IDMaterialAccent");
    auto channel = [](F32 v) { return (uint8_t)llclamp(ll_round(v * 255.f), 0, 255); };
    const idmaterial::Rgb seed{ channel(accent.mV[VRED]), channel(accent.mV[VGREEN]), channel(accent.mV[VBLUE]) };
    const bool dark = gSavedSettings.getBOOL("IDMaterialDarkMode");
    const auto scheme = idmaterial::scheme(seed, dark);

    LLUIColorTable& table = LLUIColorTable::instance();
    for (const ChromeColor& c : CHROME_COLORS)
    {
        const idmaterial::Rgb& rgb = scheme[(size_t)c.role];
        table.setOverrideColor(c.name, LLColor4(rgb.r / 255.f, rgb.g / 255.f, rgb.b / 255.f, c.alpha));
    }
    for (const FunctionalColor& c : FUNCTIONAL_COLORS)
    {
        table.setSkinColor(c.name, functionalColor(c, dark));
    }
}

LLUIColor IDMaterialTheme::getPreferenceColor(const std::string& name)
{
    return LLUIColorTable::instance().getUserOrSkinColor(name);
}

void IDMaterialTheme::setPreferenceColor(const std::string& name, const LLColor4& color)
{
    LLUIColorTable& table = LLUIColorTable::instance();
    if (isActive())
    {
        if (table.hasOverrideColor(name))
            return;
        if (const FunctionalColor* c = findFunctional(name); c && (color == functionalColor(*c, false) || color == functionalColor(*c, true)))
        {
            table.followSkinColor(name);
            return;
        }
    }
    table.setColor(name, color);
}

LLColor4 IDMaterialTheme::worldChatColor(const LLColor4& color)
{
    if (!isActive() || gSavedSettings.getBOOL("IDMaterialDarkMode"))
        return color;
    for (const char* name : WORLD_CHAT_COLORS)
    {
        if (const FunctionalColor* c = findFunctional(name); c && functionalColor(*c, false) == color)
            return functionalColor(*c, true);
    }
    return color;
}
