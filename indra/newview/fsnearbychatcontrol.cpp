/**
 * @file fsnearbychatcontrol.cpp
 * @brief Nearby chat input control implementation
 *
 * $LicenseInfo:firstyear=2009&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 * Copyright (C) 2012, Zi Ree @ Second Life
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fsnearbychatcontrol.h"
#include "fsnearbychathub.h"
#include "lfsimfeaturehandler.h"
#include "llagent.h"        // gAgent
#include "llagentcamera.h"  // gAgentCamera
#include "llautoreplace.h"
#include "llchatmentionhelper.h"
#include "llemojihelper.h"
#include "llfloaterchatmentionpicker.h"
#include "llfocusmgr.h"
#include "lllocalcliprect.h"
#include "llscrollcontainer.h"
#include "llworld.h"
#include "rlvactions.h"

static LLDefaultChildRegistry::Register<FSNearbyChatControl> r("fs_nearby_chat_control");

FSNearbyChatControl::FSNearbyChatControl(const FSNearbyChatControl::Params& p) :
    LLChatEntry(p),
    mDefault(p.is_default),
    mTextPadLeft(p.text_pad_left),
    mTextPadRight(p.text_pad_right),
    mBackgroundPad(p.background_pad),
    mBgImage(p.background_image),
    mBgImageDisabled(p.background_image_disabled),
    mBgImageFocused(p.background_image_focused)
{
    //<FS:TS> FIRE-11373: Autoreplace doesn't work in nearby chat bar
    setAutoreplaceCallback(boost::bind(&LLAutoReplace::autoreplaceCallback, LLAutoReplace::getInstance(), _1, _2, _3, _4, _5));
    setKeystrokeCallback([this](LLTextEditor* caller) { onKeystroke(caller); });
    FSNearbyChat::instance().registerChatBar(this);

    setCommitOnFocusLost(false);
    setPassDelete(true);
    mBGVisible = false;
    setFont(LLViewerChat::getChatFont());
    enableSingleLineMode(true);

    setShowChatMentionPicker(!RlvActions::isRlvEnabled() || RlvActions::canShowName(RlvActions::SNC_DEFAULT));
    mRlvBehaviorCallbackConnection = gRlvHandler.setBehaviourToggleCallback(
        boost::bind(&FSNearbyChatControl::updateRlvRestrictions, this, _1));

    setShowEmojiHelper(gSavedSettings.getBOOL("FSEnableEmojiWindowPopupWhileTyping"));
    mEmojiHelperSettingConnection =
        gSavedSettings.getControl("FSEnableEmojiWindowPopupWhileTyping")->getSignal()->connect(
            boost::bind(&FSNearbyChatControl::updateEmojiHelperSetting, this, _2));

    // Register for font change notifications
    LLViewerChat::setFontChangedCallback(boost::bind(&FSNearbyChatControl::setFont, this, _1));
}

FSNearbyChatControl::~FSNearbyChatControl()
{
    if (mRlvBehaviorCallbackConnection.connected())
    {
        mRlvBehaviorCallbackConnection.disconnect();
    }
    if (mEmojiHelperSettingConnection.connected())
    {
        mEmojiHelperSettingConnection.disconnect();
    }
    LLFloaterChatMentionPicker::removeParticipantSource(this);
}

void FSNearbyChatControl::onKeystroke(LLTextEditor* caller)
{
    FSNearbyChat::handleChatBarKeystroke(caller);
}

void FSNearbyChatControl::paste()
{
    LLChatEntry::paste();

    // Keep nearby chat on one visual line by flattening paragraph markers from paste.
    S32 cursor_pos = getCursorPos();
    LLWString content = getWText();
    LLWStringUtil::replaceChar(content, llwchar(182), llwchar('\n'));
    setWText(content);
    setCursorPos(cursor_pos);
}

// send our focus status to the LLNearbyChat hub
void FSNearbyChatControl::onFocusReceived()
{
    FSNearbyChat::instance().setFocusedInputEditor(this, true);
    LLFloaterChatMentionPicker::updateParticipantSource(this);
    LLChatEntry::onFocusReceived();
}

void FSNearbyChatControl::onFocusLost()
{
    FSNearbyChat::instance().setFocusedInputEditor(this, false);
    LLFloaterChatMentionPicker::removeParticipantSource(this);
    LLChatEntry::onFocusLost();
}

void FSNearbyChatControl::setFocus(bool focus)
{
    FSNearbyChat::instance().setFocusedInputEditor(this, focus);
    if (focus)
    {
        LLFloaterChatMentionPicker::updateParticipantSource(this);
    }
    else
    {
        LLFloaterChatMentionPicker::removeParticipantSource(this);
    }
    LLChatEntry::setFocus(focus);
}

void FSNearbyChatControl::draw()
{
    applyTextPadding();
    drawBackground();
    LLChatEntry::draw();
    drawBorderOverlay();
    drawFullHeightCursor();
}

void FSNearbyChatControl::setTextPadding(S32 left, S32 right)
{
    mTextPadLeft = left;
    mTextPadRight = right;
    applyTextPadding();
}

void FSNearbyChatControl::drawBackground()
{
    LLUIImage* image = nullptr;

    if (getReadOnly())
    {
        image = mBgImageDisabled;
    }
    else if (hasFocus())
    {
        image = mBgImageFocused;
    }
    else
    {
        image = mBgImage;
    }

    if (!image)
    {
        return;
    }

    LLRect background_rect = getLocalRect();
    background_rect.stretch(-mBackgroundPad);

    // Match LLLineEditor border alignment.
    background_rect.mTop -= 1;
    background_rect.mRight -= 1;
    if (background_rect.getWidth() <= 0 || background_rect.getHeight() <= 0)
    {
        return;
    }

    const F32 alpha = getCurrentTransparency();
    LLColor4 image_color = LLColor4::white;
    image_color.setAlpha(alpha);
    image->draw(background_rect, image_color);
}

void FSNearbyChatControl::drawBorderOverlay()
{

    LLRect background_rect = getLocalRect();
    background_rect.stretch(-mBackgroundPad);
    background_rect.mTop += 1;
    background_rect.mRight += 1;
    if (background_rect.getWidth() <= 0 || background_rect.getHeight() <= 0)
    {
        return;
    }

    const F32 alpha = getCurrentTransparency();
    if (hasFocus())
    {
        LLColor4 focus_color = gFocusMgr.getFocusColor();
        // Match the color of the usual border tint
        const F32 focus_tint_scale = 0.82f;
        const F32 focus_alpha_scale = 0.75f;
        focus_color.mV[0] *= focus_tint_scale;
        focus_color.mV[1] *= focus_tint_scale;
        focus_color.mV[2] *= focus_tint_scale;
        focus_color.setAlpha(alpha * focus_alpha_scale);

        // Draw focus outline
        const S32 focus_width = llmax(1, gFocusMgr.getFocusFlashWidth());
        for (S32 i = 0; i < focus_width; ++i)
        {
            LLRect focus_rect = background_rect;
            focus_rect.stretch(-i);
            if (focus_rect.getWidth() <= 0 || focus_rect.getHeight() <= 0)
            {
                break;
            }
            gl_rect_2d(focus_rect, focus_color, false);
        }
    }

    // LLLineEditor-style subtle inner shade ring.
    if (background_rect.getWidth() > 2 && background_rect.getHeight() > 2)
    {
        LLRect inner_border_rect = background_rect;
        inner_border_rect.stretch(-1);

        LLColor4 inner_shade = LLColor4::black;
        const F32 inner_alpha = getReadOnly() ? 0.12f : (hasFocus() ? 0.10f : 0.16f);
        inner_shade.setAlpha(alpha * inner_alpha);
        gl_rect_2d(inner_border_rect, inner_shade, false);
    }
}

void FSNearbyChatControl::drawFullHeightCursor()
{
    if (!hasFocus() || getReadOnly() || !gFocusMgr.getAppHasFocus())
    {
        return;
    }

    const F32 elapsed = mCursorBlinkTimer.getElapsedTimeF32();
    if (elapsed >= 1.0f && !(S32(elapsed * 2) & 1))
    {
        return;
    }

    LLRect cursor_rect = getLocalRectFromDocIndex(mCursorPos);
    cursor_rect.translate(-1, 0);
    cursor_rect.mRight = cursor_rect.mLeft + 2;

    LLRect content_rect = mScroller ? mScroller->getContentWindowRect() : getLocalRect();
    cursor_rect.mTop = content_rect.mTop;
    cursor_rect.mBottom = content_rect.mBottom;
    cursor_rect.intersectWith(content_rect);

    LLColor4 cursor_color = mCursorColor.get() % getDrawContext().mAlpha;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(cursor_color.mV);
    LLLocalClipRect clip(content_rect, true);
    gl_rect_2d(cursor_rect);
}

void FSNearbyChatControl::applyTextPadding()
{
    const S32 right_overlay_clip = 2;

    if (mScroller)
    {
        LLRect desired_scroller_rect = getLocalRect();
        desired_scroller_rect.mRight = llmax(desired_scroller_rect.mLeft,
                                             desired_scroller_rect.mRight - right_overlay_clip);

        if (mScroller->getRect() != desired_scroller_rect)
        {
            mScroller->setRect(desired_scroller_rect);
        }
    }

    LLRect base_rect = mScroller ? mScroller->getContentWindowRect() : getLocalRect();

    const LLRect local_rect = getLocalRect();
    if (base_rect.getHeight() < local_rect.getHeight())
    {
        base_rect.mTop = local_rect.mTop;
        base_rect.mBottom = local_rect.mBottom;
    }

    base_rect.mLeft = llmin(base_rect.mRight, base_rect.mLeft + mTextPadLeft);
    base_rect.mRight = llmax(base_rect.mLeft, base_rect.mRight - mTextPadRight);

    if (base_rect != mVisibleTextRect)
    {
        mVisibleTextRect = base_rect;
        needsReflow();
    }
}

void FSNearbyChatControl::autohide(bool after_send)
{
    if (isDefault())
    {
        const bool in_mouselook = gAgentCamera.cameraMouselook();
        const bool show_interface_in_mouselook = gSavedSettings.getBOOL("FSShowInterfaceInMouselook");
        const bool closeChatOnReturn = gSavedSettings.getBOOL("CloseChatOnReturn")
                         && !(!in_mouselook && gSavedSettings.getBOOL("FSCloseChatOnReturnInMouselook"));
        const bool autohideChatBar = gSavedSettings.getBOOL("AutohideChatBar");

        if (closeChatOnReturn)
        {
            setFocus(false);
        }

        if ((!after_send || closeChatOnReturn) &&
            ((in_mouselook && !show_interface_in_mouselook) || autohideChatBar))
        {
            FSNearbyChat::instance().showDefaultChatBar(false);
        }
    }
}

// handle ESC key here
bool FSNearbyChatControl::handleKeyHere(KEY key, MASK mask)
{
    bool handled = false;
    EChatType type = CHAT_TYPE_NORMAL;

    if (LLChatMentionHelper::instance().isActive(this) ||
        LLEmojiHelper::instance().isActive(this))
    {
        return LLChatEntry::handleKeyHere(key, mask);
    }

    // autohide the chat bar if escape key was pressed and we're the default chat bar
    if (key == KEY_ESCAPE && mask == MASK_NONE)
    {
        // we let ESC key go through to the rest of the UI code, so don't set handled = true
        autohide(false);
        gAgent.stopTyping();
    }
    else if (KEY_RETURN == key)
    {
        if (mask == MASK_CONTROL && gSavedSettings.getBOOL("FSUseCtrlShout"))
        {
            // shout
            type = CHAT_TYPE_SHOUT;
            handled = true;
        }
        else if (mask == MASK_SHIFT && gSavedSettings.getBOOL("FSUseShiftWhisper"))
        {
            // whisper
            type = CHAT_TYPE_WHISPER;
            handled = true;
        }
        else if (mask == MASK_ALT && gSavedSettings.getBOOL("FSUseAltOOC"))
        {
            // OOC
            type = CHAT_TYPE_OOC;
            handled = true;
        }
        else if (mask == (MASK_SHIFT | MASK_CONTROL))
        {
            // linefeed
            if ((wstring_utf8_length(getWText()) + wchar_utf8_length(llwchar(182))) > getMaxTextLength())
            {
                LLUI::getInstance()->reportBadKeystroke();
            }
            else
            {
                LLWString line_break(1, llwchar(182));
                insertText(line_break);
            }

            return true;
        }
        else
        {
            // say
            type = CHAT_TYPE_NORMAL;
            handled = true;
        }
    }

    if (handled)
    {
        // save current line in the history buffer
        updateHistory();

        // send chat to nearby chat hub
        FSNearbyChat::instance().sendChat(getConvertedText(), type);

        setText(LLStringExplicit(""));
        autohide(true);
        return true;
    }

    // let the line editor handle everything we don't handle
    return LLChatEntry::handleKeyHere(key, mask);
}

uuid_vec_t FSNearbyChatControl::getSessionParticipants() const
{
    if (!isAgentAvatarValid() || !LLWorld::instanceExists() || !LFSimFeatureHandler::instanceExists())
    {
        return {};
    }

    uuid_vec_t avatar_ids;
    LLWorld::instance().getAvatars(&avatar_ids, nullptr, gAgent.getPositionGlobal(), (F32)LFSimFeatureHandler::instance().sayRange());
    return avatar_ids;
}

void FSNearbyChatControl::updateRlvRestrictions(ERlvBehaviour behavior)
{
    if (behavior != RLV_BHVR_SHOWNAMES)
    {
        return;
    }

    setShowChatMentionPicker(!RlvActions::isRlvEnabled() || RlvActions::canShowName(RlvActions::SNC_DEFAULT));
}

void FSNearbyChatControl::updateEmojiHelperSetting(const LLSD& data)
{
    setShowEmojiHelper(data.asBoolean());
}
