/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.internal.policy.impl.keyguard;

import android.app.ActivityManagerNative;
import android.content.Context;
import android.content.pm.UserInfo;
import android.os.RemoteException;
import android.os.UserManager;
import android.util.AttributeSet;
import android.util.Log;
import android.view.View;
import android.view.WindowManagerGlobal;
import android.widget.FrameLayout;

import com.android.internal.R;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;

public class KeyguardMultiUserSelectorView extends FrameLayout implements View.OnClickListener {
    private static final String TAG = "KeyguardMultiUserSelectorView";

    private KeyguardSubdivisionLayout mUsersGrid;
    private KeyguardMultiUserAvatar mActiveUserAvatar;
    private KeyguardHostView.UserSwitcherCallback mCallback;
    private static final int SWITCH_ANIMATION_DURATION = 150;
    private static final int FADE_OUT_ANIMATION_DURATION = 100;

    public KeyguardMultiUserSelectorView(Context context) {
        this(context, null, 0);
    }

    public KeyguardMultiUserSelectorView(Context context, AttributeSet attrs) {
        this(context, attrs, 0);
    }

    public KeyguardMultiUserSelectorView(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);
    }

    protected void onFinishInflate () {
        init();
    }

    public void setCallback(KeyguardHostView.UserSwitcherCallback callback) {
        mCallback = callback;
    }

    public void init() {
        mUsersGrid = (KeyguardSubdivisionLayout) findViewById(R.id.keyguard_users_grid);
        mUsersGrid.removeAllViews();
        setClipChildren(false);
        setClipToPadding(false);

        UserInfo activeUser;
        try {
            activeUser = ActivityManagerNative.getDefault().getCurrentUser();
        } catch (RemoteException re) {
            activeUser = null;
        }

        UserManager mUm = (UserManager) mContext.getSystemService(Context.USER_SERVICE);
        ArrayList<UserInfo> users = new ArrayList<UserInfo>(mUm.getUsers(true));
        Collections.sort(users, mOrderAddedComparator);

        for (UserInfo user: users) {
            KeyguardMultiUserAvatar uv = createAndAddUser(user);
            if (user.id == activeUser.id) {
                mActiveUserAvatar = uv;
            }
        }
        mActiveUserAvatar.setActive(true, false, 0, null);
    }

    Comparator<UserInfo> mOrderAddedComparator = new Comparator<UserInfo>() {
        @Override
        public int compare(UserInfo lhs, UserInfo rhs) {
            return (lhs.serialNumber - rhs.serialNumber);
        }
    };

    private KeyguardMultiUserAvatar createAndAddUser(UserInfo user) {
        KeyguardMultiUserAvatar uv = KeyguardMultiUserAvatar.fromXml(
                R.layout.keyguard_multi_user_avatar, mContext, this, user);
        mUsersGrid.addView(uv);
        return uv;
    }

    @Override
    public void onClick(View v) {
        if (!(v instanceof KeyguardMultiUserAvatar)) return;
        final KeyguardMultiUserAvatar avatar = (KeyguardMultiUserAvatar) v;
        if (mActiveUserAvatar == avatar) {
            // They clicked the active user, no need to do anything
            return;
        } else {
            // Reset the previously active user to appear inactive
            avatar.lockDrawableState();
            mCallback.hideSecurityView(FADE_OUT_ANIMATION_DURATION);
            mActiveUserAvatar.setActive(false, true,  SWITCH_ANIMATION_DURATION, new Runnable() {
                @Override
                public void run() {
                    try {
                        ActivityManagerNative.getDefault().switchUser(avatar.getUserInfo().id);
                        WindowManagerGlobal.getWindowManagerService().lockNow();
                        // Set the new active user, and make it appear active
                        avatar.resetDrawableState();
                        mCallback.showSecurityView();
                        mActiveUserAvatar = avatar;
                        mActiveUserAvatar.setActive(true, false, 0, null);
                    } catch (RemoteException re) {
                        Log.e(TAG, "Couldn't switch user " + re);
                    }
                }
            });
        }
    }
}
